#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdio.h>
#include "bot_hard.h"

/* Strong Connect-4 AI (15s budget):
 * - Bitboard with sentinel row (7 bits per column)
 * - (position, mask) representation (gamesolver style)
 * - Negamax + alpha-beta + transposition table
 * - Iterative deepening with aspiration windows
 * - Heuristic: center control + window scoring
 * - Small built-in opening book for early moves
 */

#define WIN_SCORE      1000000
#define TIME_LIMIT_SEC 15.0   /* hard limit per move */
/* Set to 1 to enable per-move logging */
#define LOG_MOVES 0

/* ---- Bitboard layout ----
 * 7 bits per column
 * bit index = col * 7 + row
 * rows: 0..5 are real cells (0 bottom), 6 is sentinel
 */

static inline unsigned long long bottom_mask(int col) { return 1ULL << (col * 7); }
static inline unsigned long long top_mask(int col)    { return 1ULL << (col * 7 + 5); }
/* Zobrist hashing for TT keys */
static unsigned long long zobrist[7][6][2];
static int zobrist_ready = 0;
static int tt_initialized = 0;

static unsigned long long splitmix64(unsigned long long *x) {
    unsigned long long z = (*x += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

static void init_zobrist(void) {
    if (zobrist_ready) return;
    unsigned long long seed = 88172645463325252ULL;
    for (int c = 0; c < 7; c++) {
        for (int r = 0; r < 6; r++) {
            zobrist[c][r][0] = splitmix64(&seed);
            zobrist[c][r][1] = splitmix64(&seed);
        }
    }
    zobrist_ready = 1;
}

/* Position representation (gamesolver style):
 * - position: stones of player to move
 * - mask    : stones of both players (occupancy)
 * Opponent stones = mask ^ position
 */
typedef struct {
    unsigned long long position;
    unsigned long long mask;
} Position;

/* Forward declarations for helpers used before definition */
static int count_immediate_wins(const Position *p);

/* ----- win detection on a single bitboard ----- */
static inline int has_won_bb(unsigned long long bb) {
    unsigned long long m;

    /* vertical (shift by 1) */
    m = bb & (bb >> 1);
    if (m & (m >> 2)) return 1;

    /* horizontal (shift by 7) */
    m = bb & (bb >> 7);
    if (m & (m >> 14)) return 1;

    /* diagonal / (shift by 6) */
    m = bb & (bb >> 6);
    if (m & (m >> 12)) return 1;

    /* diagonal \ (shift by 8) */
    m = bb & (bb >> 8);
    if (m & (m >> 16)) return 1;

    return 0;
}

static inline unsigned long long opponent_bb(const Position *p) {
    return p->mask ^ p->position;
}

static inline int can_play(const Position *p, int col) {
    return (p->mask & top_mask(col)) == 0;
}

/* gamesolver-style make_move:
 * position ^= mask;            // flip player to move
 * mask     |= mask + bottom_mask(col);  // drop a piece
 */
static inline void make_move(Position *p, int col) {
    p->position ^= p->mask;
    p->mask     |= p->mask + bottom_mask(col);
}

/* any legal moves left? */
static inline int board_full_pos(const Position *p) {
    for (int c = 0; c < COLS; c++) {
        if (can_play(p, c)) return 0;
    }
    return 1;
}

/* ----- transposition table ----- */

#define TT_BITS 22              /* 4M entries */
#define TT_SIZE (1U << TT_BITS)
#define TT_MASK (TT_SIZE - 1)

typedef struct {
    unsigned long long key; /* position + mask */
    int depth;              /* remaining depth searched */
    int value;              /* score from side-to-move POV */
    char flag;              /* 0 exact, 1 lowerbound, 2 upperbound */
    int move;               /* best move (0-based column), or -1 */
    unsigned char gen;      /* generation stamp to age out stale entries */
} TTEntry;

static TTEntry ttable[TT_SIZE];
static unsigned char tt_generation = 1;

/* Killer moves + history heuristic */
#define MAX_PLY 64
static int killer_moves[2][MAX_PLY]; /* two killers per ply */
static int history_table[7];

static inline unsigned long long tt_key(const Position *p) {
    /* Zobrist over board contents, encoded as to-move vs opponent. */
    unsigned long long key = 0;
    unsigned long long player = p->position;
    unsigned long long opp    = p->mask ^ p->position;
    for (int c = 0; c < 7; c++) {
        for (int r = 0; r < 6; r++) {
            unsigned long long bit = 1ULL << (c * 7 + r);
            if (player & bit)      key ^= zobrist[c][r][0];
            else if (opp & bit)    key ^= zobrist[c][r][1];
        }
    }
    return key;
}

static inline int tt_idx_u64(unsigned long long k) {
    return (int)(k & TT_MASK);
}

static void init_tt(void) {
    if (tt_initialized) return;
    for (int i = 0; i < TT_SIZE; i++) {
        ttable[i].key = 0;
        ttable[i].depth = -1;
        ttable[i].value = 0;
        ttable[i].flag = 0;
        ttable[i].move = -1;
        ttable[i].gen  = 0;
    }
    tt_initialized = 1;
}

/* center-first move order */
static const int ORDER[7] = {3, 2, 4, 1, 5, 0, 6};

static void gen_moves(const Position *p, int moves[7], int *count) {
    (void)p; /* we only need can_play, which uses p */
    *count = 0;
    for (int i = 0; i < COLS; i++) {
        int c = ORDER[i];
        if (can_play(p, c)) moves[(*count)++] = c;
    }
}

/* ----- heuristic evaluation (window-based) ----- */

/* cell_at:
 *  return +1 if current player stone
 *         -1 if opponent stone
 *          0 if empty
 */
static inline int cell_at(const Position *p, int r, int c) {
    unsigned long long bit = 1ULL << (c * 7 + r);
    if (p->position & bit)      return 1;
    if (opponent_bb(p) & bit)   return -1;
    return 0;
}

/* Evaluate one 4-cell window starting at (r,c) with direction (dr,dc) */
static int eval_window(const Position *p, int r, int c, int dr, int dc) {
    /* weights for 1, 2, 3 in a row (4-in-a-row handled by search) */
    static const int W[4] = {0, 2, 12, 60};
    int pc = 0, oc = 0;
    for (int i = 0; i < 4; i++) {
        int rr = r + i * dr;
        int cc = c + i * dc;
        int v = cell_at(p, rr, cc);
        if (v == 1) pc++;
        else if (v == -1) oc++;
    }
    if (pc && oc) return 0;     /* contested window */
    if (pc) return W[pc];
    if (oc) return -W[oc];
    return 0;
}

static int evaluate_position_internal(const Position *p, int include_threats) {
    int score = 0;

    /* center control: column 3 is strongest */
    for (int r = 0; r < 6; r++) {
        int v = cell_at(p, r, 3);
        score += v * 4;
    }

    /* horizontal windows */
    for (int r = 0; r < 6; r++) {
        for (int c = 0; c <= 3; c++) {
            score += eval_window(p, r, c, 0, 1);
        }
    }
    /* vertical windows */
    for (int r = 0; r <= 2; r++) {
        for (int c = 0; c < 7; c++) {
            score += eval_window(p, r, c, 1, 0);
        }
    }
    /* diagonal / (down-right) */
    for (int r = 0; r <= 2; r++) {
        for (int c = 0; c <= 3; c++) {
            score += eval_window(p, r, c, 1, 1);
        }
    }
    /* diagonal \ (down-left) */
    for (int r = 0; r <= 2; r++) {
        for (int c = 3; c < 7; c++) {
            score += eval_window(p, r, c, 1, -1);
        }
    }

    if (include_threats) {
        /* Threat and double-threat awareness */
        int threats_me = count_immediate_wins(p);
        Position opp_view = *p;
        opp_view.position ^= opp_view.mask;
        int threats_opp = count_immediate_wins(&opp_view);

        score += (threats_me - threats_opp) * 200;
        if (threats_me >= 2) score += 5000;   /* double threat for current player */
        if (threats_opp >= 2) score -= 5000;  /* opponent double threat is scary */
    }

    return score;
}

static inline int evaluate_position(const Position *p) {
    return evaluate_position_internal(p, 1);
}

/* ----- timing ----- */

static inline int timed_out(clock_t start_time) {
    return ((double)(clock() - start_time) / (double)CLOCKS_PER_SEC) >= TIME_LIMIT_SEC;
}

/* ----- quick immediate win detection for current player ----- */
static int count_immediate_wins(const Position *p) {
    int wins = 0;
    for (int c = 0; c < COLS; c++) {
        if (!can_play(p, c)) continue;
        Position tmp = *p;
        make_move(&tmp, c);
        if (has_won_bb(opponent_bb(&tmp))) wins++;
    }
    return wins;
}

/* ----- small opening book (board-based) ----- */
/* Returns 1-based column index, or 0 if no book move. */
static int opening_book_move(char board[ROWS][COLS], char bot) {
    int stones = 0;
    int my_stones = 0;
    int opp_stones = 0;

    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
            char cell = board[r][c];
            if (cell != '.') {
                stones++;
                if (cell == bot) my_stones++;
                else opp_stones++;
            }
        }
    }

    /* Only use book very early (first 2–3 plies) */
    if (stones > 4) return 0;

    /* First move of the game: always take center if free */
    if (stones == 0) {
        if (board[ROWS - 1][3] == '.') return 4; /* column 4 (index 3) */
        return 0;
    }

    /* If we're second player and opponent opened somewhere */
    if (stones == 1 && my_stones == 0 && opp_stones == 1) {
        int opp_col = -1;
        for (int c = 0; c < COLS; c++) {
            for (int r = ROWS - 1; r >= 0; r--) {
                if (board[r][c] != '.') {
                    opp_col = c;
                    goto found_opp;
                }
            }
        }
    found_opp:
        if (opp_col == -1) return 0;

        /* If opponent did NOT play center, we take center. */
        if (opp_col != 3 && board[ROWS - 1][3] == '.') {
            return 4;
        } else if (opp_col == 3) {
            /* Opponent played center; reply next to center (col 3 or 5) */
            if (board[ROWS - 1][2] == '.') return 3;
            if (board[ROWS - 1][4] == '.') return 5;
        }
        return 0;
    }

    /* If we opened center and opponent replied, we keep playing near center. */
    if (stones == 2 && my_stones == 1 && opp_stones == 1) {
        /* If we already have center, take one of the adjacent columns. */
        if (board[ROWS - 1][3] == bot) {
            if (board[ROWS - 1][2] == '.') return 3;
            if (board[ROWS - 1][4] == '.') return 5;
        }
        return 0;
    }

    return 0;
}

/* ----- negamax with alpha-beta + TT + time limit ----- */

static int negamax(Position *p,
                   int depth_rem,
                   int max_depth,
                   int alpha,
                   int beta,
                   clock_t start_time,
                   int *timeout_flag,
                   int *node_counter)
{
    if (*timeout_flag) return 0;

    /* periodic time check to reduce overhead */
    if (node_counter) {
        (*node_counter)++;
        if (((*node_counter) & 0x7F) == 0 && timed_out(start_time)) {
            *timeout_flag = 1;
            return 0;
        }
    } else if (timed_out(start_time)) {
        *timeout_flag = 1;
        return 0;
    }

    /* terminal: opponent (last mover) has 4 in a row */
    if (has_won_bb(opponent_bb(p))) {
        /* losing position for side-to-move; prefer later losses slightly */
        return -WIN_SCORE + (max_depth - depth_rem);
    }

    /* draw or depth cutoff */
    if (depth_rem == 0 || board_full_pos(p)) {
        /* quick tactical check even at leaf */
        int my_wins = count_immediate_wins(p);
        if (my_wins > 0) return WIN_SCORE - (max_depth - depth_rem);

        /* opponent immediate wins cost big */
        Position opp = *p;
        opp.position ^= opp.mask;
        int opp_wins = count_immediate_wins(&opp);
        if (opp_wins > 0) return -WIN_SCORE + (max_depth - depth_rem);

        return evaluate_position_internal(p, 0);
    }

    /* TT lookup */
    unsigned long long key = tt_key(p);
    TTEntry *e = &ttable[tt_idx_u64(key)];

    if (e->gen == tt_generation && e->key == key && e->depth >= depth_rem) {
        if (e->flag == 0) {
            /* exact score */
            return e->value;
        } else if (e->flag == 1 && e->value > alpha) {
            alpha = e->value;        /* lower bound */
        } else if (e->flag == 2 && e->value < beta) {
            beta = e->value;         /* upper bound */
        }
        if (alpha >= beta) {
            return e->value;
        }
    }

    /* record bounds AFTER TT tightening */
    int alpha0 = alpha;
    int beta0  = beta;

    int moves[7], mcount = 0;
    gen_moves(p, moves, &mcount);
    if (mcount == 0) return 0;

    int ply = max_depth - depth_rem;
    if (ply >= MAX_PLY) ply = MAX_PLY - 1;

    /* If TT has a stored best move, put it first */
    if (e->gen == tt_generation && e->key == key && e->depth > 0 && e->move >= 0 && e->move < COLS) {
        for (int i = 0; i < mcount; i++) {
            if (moves[i] == e->move) {
                int tmp = moves[0];
                moves[0] = moves[i];
                moves[i] = tmp;
                break;
            }
        }
    }

    /* simple move ordering: prefer closer to center + light eval */
    int scores[7];
    /* Precompute opponent immediate wins for block priority */
    Position opp_view = *p;
    opp_view.position ^= opp_view.mask;
    int opp_winning_cols[7];
    int opp_wcount = 0;
    for (int oc = 0; oc < COLS; oc++) {
        if (!can_play(&opp_view, oc)) continue;
        Position t = opp_view;
        make_move(&t, oc);
        if (has_won_bb(opponent_bb(&t))) {
            opp_winning_cols[opp_wcount++] = oc;
        }
    }

    for (int i = 0; i < mcount; i++) {
        int c = moves[i];
        int s = 6 - (c > 3 ? c - 3 : 3 - c);   /* center bias */

        Position child = *p;
        make_move(&child, c);
        /* If this move wins immediately, boost its priority heavily */
        if (has_won_bb(opponent_bb(&child))) {
            s += 12000;
        }
        /* If this move blocks an opponent immediate win, boost */
        for (int k = 0; k < opp_wcount; k++) {
            if (opp_winning_cols[k] == c) { s += 8000; break; }
        }
        /* If this move prevents opponent immediate win, boost modestly */
        Position opp_view_child = child;
        opp_view_child.position ^= opp_view_child.mask; /* flip POV */
        int opp_can_win = 0;
        for (int oc = 0; oc < COLS; oc++) {
            if (!can_play(&opp_view_child, oc)) continue;
            Position t = opp_view_child;
            make_move(&t, oc);
            if (has_won_bb(opponent_bb(&t))) { opp_can_win = 1; break; }
        }
        if (!opp_can_win) s += 200;

        s += evaluate_position_internal(&child, 0) / 32;   /* cheap heuristic lookahead */
        /* history heuristic */
        s += history_table[c] / 8;
        /* killer moves */
        if (killer_moves[0][ply] == c) s += 2000;
        else if (killer_moves[1][ply] == c) s += 1000;

        if (e->key == key && e->move == c) s += 1000; /* strongly prefer TT move */

        scores[i] = s;
    }
    /* sort by scores (descending) – simple selection sort */
    for (int i = 0; i < mcount - 1; i++) {
        int best = i;
        for (int j = i + 1; j < mcount; j++) {
            if (scores[j] > scores[best]) best = j;
        }
        if (best != i) {
            int ts = scores[i]; scores[i] = scores[best]; scores[best] = ts;
            int tm = moves[i];  moves[i]  = moves[best]; moves[best]  = tm;
        }
    }

    int best_val = -WIN_SCORE;
    int best_move_local = moves[0];

    for (int i = 0; i < mcount; i++) {
        int col = moves[i];
        Position child = *p;
        make_move(&child, col);

        /* Late-move pruning for shallow nodes */
        if (depth_rem <= 2 && i >= 3 && scores[i] + 200 < alpha) {
            continue;
        }

        int score = -negamax(&child,
                             depth_rem - 1,
                             max_depth,
                             -beta,
                             -alpha,
                             start_time,
                             timeout_flag,
                             node_counter);

        if (*timeout_flag) return 0;

        if (score > best_val) {
            best_val = score;
            best_move_local = col;
        }
        if (score > alpha) alpha = score;
        if (alpha >= beta) {
            /* update killers/history on cut-off */
            int bonus = depth_rem * depth_rem;
            if (bonus > 256) bonus = 256;
            history_table[col] += bonus;
            if (killer_moves[0][ply] != col) {
                killer_moves[1][ply] = killer_moves[0][ply];
                killer_moves[0][ply] = col;
            }
            break;
        }
    }

    /* store in TT (prefer deeper info on collisions) */
    if (!(e->gen == tt_generation && e->key == key && e->depth > depth_rem)) {
        e->key   = key;
        e->depth = depth_rem;
        e->value = best_val;
        e->move  = best_move_local;
        e->gen   = tt_generation;
    }
    if (best_val <= alpha0)      e->flag = 2; /* upper bound */
    else if (best_val >= beta0) e->flag = 1; /* lower bound */
    else                        e->flag = 0; /* exact */

    return best_val;
}

/* ==================== public entry ==================== */

int getBotMoveHard(char board[ROWS][COLS], char bot, char opponent) {
    (void)opponent; /* board contents + bot char define everything */
    init_zobrist();

    /* Build Position: position = stones of side to move (bot),
       mask = all stones */
    init_tt();
    for (int i = 0; i < MAX_PLY; i++) {
        killer_moves[0][i] = -1;
        killer_moves[1][i] = -1;
    }
    for (int i = 0; i < 7; i++) history_table[i] = 0;
    Position pos;
    pos.position = 0;
    pos.mask     = 0;

    for (int c = 0; c < COLS; c++) {
        for (int r = 0; r < ROWS; r++) {
            char cell = board[r][c];
            if (cell == '.') continue;
            int row_from_bottom = ROWS - 1 - r;
            int idx = c * 7 + row_from_bottom;
            unsigned long long bit = 1ULL << idx;
            pos.mask |= bit;
            if (cell == bot) {
                pos.position |= bit;
            }
        }
    }

    /* Advance TT generation to age out old entries without a full clear */
    tt_generation++;
    if (tt_generation == 0) tt_generation = 1; /* avoid 0 to keep "unused" distinct */

    /* Opening book (only very early) */
    int ob_move = opening_book_move(board, bot);
    if (ob_move >= 1 && ob_move <= COLS && board[0][ob_move - 1] == '.') {
        return ob_move;
    }

    /* Immediate win for bot: try playing in each column */
    for (int c = 0; c < COLS; c++) {
        if (!can_play(&pos, c)) continue;
        Position tmp = pos;
        make_move(&tmp, c);
        if (has_won_bb(opponent_bb(&tmp))) {
            return c + 1;  /* 1-based column index */
        }
    }

    /* Immediate block: if opponent can win next move, block it */
    for (int c = 0; c < COLS; c++) {
        if (!can_play(&pos, c)) continue;
        Position tmp = pos;

        /* simulate opponent point of view first:
         * flip "player to move", then drop piece
         */
        tmp.position ^= tmp.mask;
        make_move(&tmp, c);
        if (has_won_bb(opponent_bb(&tmp))) {
            return c + 1;
        }
    }

    /* iterative deepening with time limit + aspiration windows */
    int empties = 0;
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++)
            if (board[r][c] == '.') empties++;

    int depth_cap = empties;        /* theoretical maximum remaining ply */

    clock_t start_time = clock();
    int timeout_flag = 0;
    int node_counter = 0;
    int best_move = 4;              /* default center (column index 3 -> return 4) */
    int last_completed_depth = 0;
    int last_score = 0;
    unsigned long long root_key = tt_key(&pos);
    TTEntry *root_entry = &ttable[tt_idx_u64(root_key)];

    if (!can_play(&pos, 3)) {       /* if center full, choose first legal */
        for (int c = 0; c < COLS; c++) {
            if (can_play(&pos, c)) { best_move = c + 1; break; }
        }
    }

    for (int depth = 1; depth <= depth_cap; depth++) {
        int local_best_move = best_move;
        int best_val_for_depth = -WIN_SCORE;
        timeout_flag = 0;

        int moves[7], mcount = 0;
        gen_moves(&pos, moves, &mcount);
        /* Root TT move ordering */
        if (root_entry->gen == tt_generation && root_entry->key == root_key && root_entry->depth > 0 &&
            root_entry->move >= 0 && root_entry->move < COLS) {
            for (int i = 0; i < mcount; i++) {
                if (moves[i] == root_entry->move) {
                    int tmp = moves[0];
                    moves[0] = moves[i];
                    moves[i] = tmp;
                    break;
                }
            }
        }
        if (mcount == 0) break;

        for (int i = 0; i < mcount; i++) {
            int col = moves[i];
            Position child = pos;
            make_move(&child, col);

            int alpha = -WIN_SCORE;
            int beta  = WIN_SCORE;

            /* aspiration windows for depth >= 3 */
            if (depth >= 3) {
                const int W = 64;
                alpha = last_score - W;
                beta  = last_score + W;
            }

            int score;
            while (1) {
                if (alpha < -WIN_SCORE) alpha = -WIN_SCORE;
                if (beta  > WIN_SCORE)  beta  = WIN_SCORE;

                score = -negamax(&child,
                                 depth - 1,
                                 depth,
                                 -beta,
                                 -alpha,
                                 start_time,
                                 &timeout_flag,
                                 &node_counter);

                if (timeout_flag) break;

                /* expand window on fail-low / fail-high once */
                if (score <= alpha && alpha > -WIN_SCORE && depth >= 3) {
                    alpha -= 256;
                    continue;
                } else if (score >= beta && beta < WIN_SCORE && depth >= 3) {
                    beta += 256;
                    continue;
                }

                break;
            }

            if (timeout_flag) break;

            if (score > best_val_for_depth) {
                best_val_for_depth = score;
                local_best_move = col + 1; /* store as 1-based */
            }
        }

        if (timeout_flag) break;
        best_move = local_best_move;
        last_completed_depth = depth;
        last_score = best_val_for_depth;

        if (timed_out(start_time)) break;
    }

    /* safety: ensure chosen column is legal */
    if (best_move < 1 || best_move > COLS || board[0][best_move - 1] != '.') {
        for (int c = 0; c < COLS; c++) {
            if (can_play(&pos, c)) { best_move = c + 1; break; }
        }
    }

    #if LOG_MOVES
    double elapsed = (double)(clock() - start_time) / (double)CLOCKS_PER_SEC;
    printf("[Hard bot] column %d | depth %d | time %.3fs\n",
           best_move, last_completed_depth, elapsed);
    #endif

    return best_move;
}
