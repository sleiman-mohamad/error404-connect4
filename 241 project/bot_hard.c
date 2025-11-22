#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdio.h>
#include "bot_hard.h"

/* Strong Connect-4 AI (15s budget):
 * - Bitboard with sentinel row (7 bits per column)
 * - Explicit P0/P1 bitboards + incremental Zobrist key
 * - Negamax + alpha-beta + transposition table
 * - Iterative deepening with aspiration windows
 * - Heuristic: center control + window scoring
 * - Small built-in opening book for early moves
 */

#define WIN_SCORE      1000000
#define TIME_LIMIT_SEC 15.0   /* hard limit per move */
/* Set to 1 to enable per-move logging */
#define LOG_MOVES 1

/* ---- Bitboard layout ----
 * 7 bits per column
 * bit index = col * 7 + row
 * rows: 0..5 are real cells (0 bottom), 6 is sentinel
 */

static inline unsigned long long bottom_mask(int col) { return 1ULL << (col * 7); }
static inline unsigned long long top_mask(int col)    { return 1ULL << (col * 7 + 5); }
/* Zobrist hashing for TT keys */
static unsigned long long zobrist[7][6][2];
static unsigned long long zobrist_side = 0;
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
    zobrist_side = splitmix64(&seed);
    zobrist_ready = 1;
}

/* Position representation (explicit):
 * - bb[0], bb[1]: stones for player 0 and player 1
 * - player_to_move: 0 or 1
 * - zkey: incremental zobrist including side-to-move
 */
typedef struct {
    unsigned long long bb[2];
    unsigned long long zkey;
    int player_to_move;
} Position;

/* Forward declarations for helpers used before definition */
static int count_immediate_wins(const Position *p);
static int list_immediate_wins(const Position *p, int cols_out[7]);
static inline int timed_out(clock_t start_time);
static inline void flip_player(Position *p);
static inline int popcount_u64(unsigned long long x);
static inline int column_height(unsigned long long occ, int col);
static int parity_score(const Position *p);
static int player_side_three_threat(const Position *p, int player, int *col_out);
static int creates_side_fork(Position *p, int my_move_col);
static const int VCF_MAX_DEPTH = 24;

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
    return p->bb[p->player_to_move ^ 1];
}

static inline unsigned long long mask_all(const Position *p) {
    return p->bb[0] | p->bb[1];
}

static inline int can_play(const Position *p, int col) {
    return (mask_all(p) & top_mask(col)) == 0;
}

static inline int col_full(const Position *p, int col) {
    return !can_play(p, col);
}

/* Heuristic: consider center diagonals "secured" if we already have support
 * in column 4 and one of 3 or 5 at row 2 or higher for the current POV.
 * This is a conservative proxy; it does not attempt a full proof.
 */
static int diagonal_secured_center(const Position *p) {
    unsigned long long mine = p->bb[p->player_to_move];
    for (int row = 2; row <= 5; row++) {
        unsigned long long b3 = 1ULL << (3 * 7 + row);
        unsigned long long b4 = 1ULL << (4 * 7 + row);
        unsigned long long b5 = 1ULL << (5 * 7 + row);
        if ((mine & b4) && ((mine & b3) || (mine & b5))) {
            return 1;
        }
    }
    return 0;
}

static inline int empties_on_board(const Position *p) {
    /* 42 playable cells on a 7x6 board */
    unsigned long long occ = mask_all(p);
    int bits = 0;
    while (occ) { occ &= (occ - 1); bits++; }
    return 42 - bits;
}

static inline void make_move(Position *p, int col) {
    unsigned long long occ = mask_all(p);
    unsigned long long move_bit = (occ + bottom_mask(col)) & ~occ;

    /* derive row from bit index */
    unsigned long long tmp = move_bit;
    int idx = 0;
    while ((tmp & 1ULL) == 0) { tmp >>= 1; idx++; }
    int row = idx - col * 7;

    int player = p->player_to_move;
    p->bb[player] |= move_bit;
    /* update zobrist incrementally */
    if (row < 6) {
        p->zkey ^= zobrist[col][row][player];
    }
    flip_player(p);
}

static inline void flip_player(Position *p) {
    p->player_to_move ^= 1;
    p->zkey ^= zobrist_side;
}

/* For viewpoint-only flips where we don't want to touch the zobrist key */
static inline void flip_view(Position *p) {
    p->player_to_move ^= 1;
}

static inline int popcount_u64(unsigned long long x) {
    int c = 0;
    while (x) { x &= (x - 1); c++; }
    return c;
}

static inline int column_height(unsigned long long occ, int col) {
    unsigned long long col_mask = 0x3FULL << (col * 7); /* 6 bits + sentinel */
    return popcount_u64(occ & col_mask);
}

/* Can the given player stack to exactly 3 stones next move in a side column? */
static int player_can_stack_to_three(const Position *p, int player, int col) {
    unsigned long long occ = mask_all(p);
    if (occ & top_mask(col)) return 0; /* column full */
    int height = column_height(occ, col);
    if (height < 2) return 0;          /* need at least two stones already */
    /* check all occupied cells belong to the player and are contiguous from bottom */
    unsigned long long shifted = p->bb[player] >> (col * 7);
    unsigned long long mask = (1ULL << height) - 1ULL; /* bottom "height" bits */
    return (shifted & mask) == mask;   /* all occupied cells belong to player */
}

/* Detect side-column (1,2,6,7 in human terms) vertical-3 threats for player */
static int player_side_three_threat(const Position *p, int player, int *col_out) {
    static const int SIDE_COLS[4] = {0, 1, 5, 6};
    for (int i = 0; i < 4; i++) {
        int c = SIDE_COLS[i];
        if (player_can_stack_to_three(p, player, c)) {
            if (col_out) *col_out = c;
            return 1;
        }
    }
    return 0;
}

/* any legal moves left? */
static inline int board_full_pos(const Position *p) {
    for (int c = 0; c < COLS; c++) {
        if (can_play(p, c)) return 0;
    }
    return 1;
}

/* ----- transposition table ----- */

#define TT_BITS 25              /* 4M entries */
#define TT_SIZE (1U << TT_BITS)
#define TT_MASK (TT_SIZE - 1)

typedef struct {
    unsigned long long key; /* zobrist key */
    int depth;              /* remaining depth searched */
    int value;              /* score from side-to-move POV */
    char flag;              /* 0 exact, 1 lowerbound, 2 upperbound */
    int move;               /* best move (0-based column), or -1 */
    unsigned char gen;      /* generation stamp to age out stale entries */
} TTEntry;

static TTEntry ttable[TT_SIZE];
static unsigned char tt_generation = 1;

/* Lightweight TT for VCF/VCT search */
#define VCF_TT_BITS 21          /* 256K entries */
#define VCF_TT_SIZE (1U << VCF_TT_BITS)
#define VCF_TT_MASK (VCF_TT_SIZE - 1)
typedef struct {
    unsigned long long key;
    unsigned char depth;   /* depth searched */
    char result;           /* -1 loss, 0 unknown, 1 win */
    unsigned char gen;
} VCFEntry;
static VCFEntry vcf_table[VCF_TT_SIZE];
static unsigned char vcf_generation = 1;

/* Killer moves + history heuristic */
#define MAX_PLY 64
static int killer_moves[2][MAX_PLY]; /* two killers per ply */
static int history_table[7];

static inline unsigned long long tt_key(const Position *p) {
    return p->zkey;
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
    for (int i = 0; i < VCF_TT_SIZE; i++) {
        vcf_table[i].key = 0;
        vcf_table[i].depth = 0;
        vcf_table[i].result = 0;
        vcf_table[i].gen = 0;
    }
    tt_initialized = 1;
}

static inline int vcf_idx(unsigned long long k) {
    return (int)(k & VCF_TT_MASK);
}

static inline int vcf_probe(const Position *p, int depth_rem) {
    VCFEntry *e = &vcf_table[vcf_idx(p->zkey)];
    if (e->gen == vcf_generation && e->key == p->zkey && e->depth >= depth_rem) {
        return (int)e->result;
    }
    return 0;
}

static inline void vcf_store(const Position *p, int depth_rem, int result) {
    VCFEntry *e = &vcf_table[vcf_idx(p->zkey)];
    if (e->gen != vcf_generation || depth_rem >= e->depth) {
        e->key = p->zkey;
        e->depth = (unsigned char)depth_rem;
        e->result = (char)result;
        e->gen = vcf_generation;
    }
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
    if (p->bb[p->player_to_move] & bit)      return 1;
    if (p->bb[p->player_to_move ^ 1] & bit)  return -1;
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

    /* edge control: avoid giving away edge columns */
    for (int c = 0; c < 7; c += 6) { /* columns 0 and 6 */
        for (int r = 0; r < 6; r++) {
            int v = cell_at(p, r, c);
            score += v * -2;          /* we prefer not to occupy edges early */
            if (v == -1) score -= 4;   /* opponent owning edges is even worse */
        }
    }

    /* parity / tempo bias: odd remaining cells in a column favor side to move */
    score += parity_score(p);

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
        flip_view(&opp_view);
        int threats_opp = count_immediate_wins(&opp_view);

        score += (threats_me - threats_opp) * 200;
        if (threats_me >= 2) score += 5000;   /* double threat for current player */
        if (threats_opp >= 2) score -= 5000;  /* opponent double threat is scary */

        /* Side-column vertical-3 danger (opponent) */
        int opp = p->player_to_move ^ 1;
        if (player_side_three_threat(p, opp, NULL)) {
            score -= 800; /* steer away from letting opponent build the trap */
        }
    }

    return score;
}

static inline int evaluate_position(const Position *p) {
    return evaluate_position_internal(p, 1);
}

/* ----- parity-aware heuristic ----- */
static int parity_score(const Position *p) {
    int s = 0;
    unsigned long long occ = mask_all(p);
    for (int c = 0; c < COLS; c++) {
        int h = column_height(occ, c);
        if (h >= 6) continue;
        int remaining = 6 - h;
        /* odd remaining cells -> side to move gets the top cell */
        int parity_bonus = (remaining & 1) ? 30 : -30;
        /* weight center columns slightly more */
        int center_weight = 3 - (c > 3 ? c - 3 : 3 - c); /* 3,2,1,0,1,2,3 */
        s += parity_bonus * (center_weight + 1);

        /* bottom threat: if this is the last cell and we win by playing here */
        if (remaining == 1) {
            Position tmp = *p;
            make_move(&tmp, c);
            if (has_won_bb(opponent_bb(&tmp))) s += 200;
            Position opp = *p;
            flip_view(&opp);
            make_move(&opp, c);
            if (has_won_bb(opponent_bb(&opp))) s -= 200;
        }
    }
    return s;
}

/* ----- threat-space search (VCF/VCF2 style) ----- */
/* Return 1 if side to move has a forced win within depth_rem plies,
 * -1 if forced loss, 0 unknown.
 * VCF2: if we create a double threat, it's a forced win.
 */
static int threat_space_search(Position *p, int depth_rem, clock_t start_time, int *timeout_flag) {
    if (*timeout_flag) return 0;
    if (depth_rem <= 0) return 0;
    if (timed_out(start_time)) { *timeout_flag = 1; return 0; }

    /* TT probe */
    int cached = vcf_probe(p, depth_rem);
    if (cached != 0) return cached;

    /* If opponent already has 4 (previous mover), this node is losing. */
    if (has_won_bb(opponent_bb(p))) return -1;

    int moves[7], mcount = 0;
    gen_moves(p, moves, &mcount);
    if (mcount == 0) return 0;

    /* If opponent has immediate wins now, restrict us to blocking them. */
    Position opp_now = *p;
    flip_view(&opp_now);
    int opp_win_cols[7];
    int opp_win_count = list_immediate_wins(&opp_now, opp_win_cols);
    if (opp_win_count > 0) {
        int filtered[7], fcount = 0;
        for (int i = 0; i < opp_win_count; i++) {
            if (can_play(p, opp_win_cols[i])) filtered[fcount++] = opp_win_cols[i];
        }
        if (fcount == 0) {
            vcf_store(p, depth_rem, -1);
            return -1;
        }
        mcount = fcount;
        for (int i = 0; i < fcount; i++) moves[i] = filtered[i];
    }

    /* Immediate win available? */
    for (int i = 0; i < mcount; i++) {
        Position child = *p;
        make_move(&child, moves[i]);
        if (has_won_bb(opponent_bb(&child))) {
            vcf_store(p, depth_rem, 1);
            return 1;
        }
    }

    /* Try to prove a forced win by chaining threats:
     * - If we make a double threat, we win (VCF2).
     * - If single threat, opponent must block that square; all blocks must fail.
     */
    int best_result = 0;
    for (int i = 0; i < mcount; i++) {
        Position child = *p;
        make_move(&child, moves[i]);

        /* our threats after this move (flip POV back to us) */
        Position my_view = child;
        flip_view(&my_view);
        int win_cols[7];
        int wcount = list_immediate_wins(&my_view, win_cols);
        if (wcount == 0) {
            /* VCT extension: if no immediate threat, see if all opponent replies still lose
             * to a threat win within remaining depth.
             */
            if (depth_rem >= 4) {
                int opp_moves[7], ocount = 0;
                gen_moves(&child, opp_moves, &ocount);
                int opp_can_escape = 0;
                for (int j = 0; j < ocount; j++) {
                    Position reply = child;
                    make_move(&reply, opp_moves[j]);
                    if (has_won_bb(opponent_bb(&reply))) { opp_can_escape = 1; break; }
                    int res = threat_space_search(&reply, depth_rem - 2, start_time, timeout_flag);
                    if (*timeout_flag) return 0;
                    if (res != 1) { opp_can_escape = 1; break; }
                }
                if (!opp_can_escape) {
                    vcf_store(p, depth_rem, 1);
                    return 1;
                }
            }
            continue; /* not forcing enough otherwise */
        }

        /* if any winning col is unplayable by opponent, it's a forced win */
        int opponent_blocks[7];
        int blockable = 0;
        for (int k = 0; k < wcount; k++) {
            if (!can_play(&child, win_cols[k])) {
                vcf_store(p, depth_rem, 1);
                return 1;
            }
            opponent_blocks[blockable++] = win_cols[k];
        }

        /* double threat -> forced win (opponent can only play one move) */
        if (blockable >= 2) {
            vcf_store(p, depth_rem, 1);
            return 1;
        }

        /* opponent must block one of the winning cols; verify all replies fail */
        int opp_cannot_escape = 1;

            /* opponent immediate winning replies pruning (ladder-like) */
            for (int b = 0; b < blockable; b++) {
                Position reply = child;
                make_move(&reply, opponent_blocks[b]);
                if (has_won_bb(opponent_bb(&reply))) { opp_cannot_escape = 0; break; }

                /* if opponent after blocking gains multiple immediate wins, treat as escape */
                Position our_again = reply;
                flip_view(&our_again);
                int opp_wins_after[7];
                int opp_wins_cnt = list_immediate_wins(&our_again, opp_wins_after);
            if (opp_wins_cnt >= 2) { opp_cannot_escape = 0; break; }
            if (opp_wins_cnt == 1 && !can_play(&reply, opp_wins_after[0])) {
                /* illegal block, ignore */
            }

            int res = threat_space_search(&reply, depth_rem - 2, start_time, timeout_flag);
            if (*timeout_flag) return 0;
            if (res != 1) { opp_cannot_escape = 0; break; }
        }
        if (opp_cannot_escape) {
            vcf_store(p, depth_rem, 1);
            return 1;
        }
    }

    /* If we could not prove a win, check if opponent has a forced win within remaining depth. */
    if (depth_rem > 1) {
        Position opp = *p;
        /* We are about to probe/store in TT in the recursive call; keep zkey consistent. */
        flip_player(&opp);
        int res = threat_space_search(&opp, depth_rem - 1, start_time, timeout_flag);
        if (*timeout_flag) return 0;
        if (res == 1) {
            vcf_store(p, depth_rem, -1);
            return -1;
        }
    }

    vcf_store(p, depth_rem, best_result);
    return best_result;
}

/* Root helper: return 1-based column of forced win, or 0 if none/unknown. */
static int threat_space_root(const Position *p, int depth_limit, clock_t start_time) {
    int timeout_flag = 0;
    int moves[7], mcount = 0;
    gen_moves(p, moves, &mcount);
    for (int i = 0; i < mcount; i++) {
        Position child = *p;
        make_move(&child, moves[i]);
        if (has_won_bb(opponent_bb(&child))) return moves[i] + 1;

        int opp_moves[7], ocount = 0;
        gen_moves(&child, opp_moves, &ocount);
        int opp_can_escape = 0;
        /* if our move already created a double threat, it's winning */
        Position my_view = child;
        flip_view(&my_view);
        int win_cols[7];
        int wcount = list_immediate_wins(&my_view, win_cols);
        if (wcount >= 2) return moves[i] + 1;

        for (int j = 0; j < ocount; j++) {
            Position reply = child;
            make_move(&reply, opp_moves[j]);
            if (has_won_bb(opponent_bb(&reply))) { opp_can_escape = 1; break; }
            int res = threat_space_search(&reply, depth_limit - 2, start_time, &timeout_flag);
            if (timeout_flag) break;
            if (res != 1) { opp_can_escape = 1; break; }
        }
        if (timeout_flag) break;
        if (!opp_can_escape) return moves[i] + 1;
    }
    return 0;
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
/* collect columns that win immediately for side-to-move; returns count */
static int list_immediate_wins(const Position *p, int cols_out[7]) {
    int wins = 0;
    for (int c = 0; c < COLS; c++) {
        if (!can_play(p, c)) continue;
        Position tmp = *p;
        make_move(&tmp, c);
        if (has_won_bb(opponent_bb(&tmp))) {
            cols_out[wins++] = c;
        }
    }
    return wins;
}

/* Opening book disabled: always fall back to search. */
static int opening_book_move(char board[ROWS][COLS], char bot) {
    (void)bot;
    /* If board is empty, open in center (column 4). Otherwise, defer to search. */
    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
            if (board[r][c] != '.') return 0;
        }
    }
    return 4;
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
        flip_view(&opp);
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
    flip_view(&opp_view);
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
        int s = 6 - (c > 3 ? c - 3 : 3 - c);
            /* skip moves that create fork-loss pattern */
    if (creates_side_fork(p, c)) {
        scores[i] = -1000000000;
        continue;
    }
   /* center bias */

        Position child = *p;
        make_move(&child, c);
        /* If this move wins immediately, boost its priority heavily */
        if (has_won_bb(opponent_bb(&child))) {
            s += 5000;  /* immediate win */
        }
        /* If this move blocks an opponent immediate win, boost */
        for (int k = 0; k < opp_wcount; k++) {
            if (opp_winning_cols[k] == c) { s += 2000; break; }
        }
        /* If this move allows opponent to stack to 3 in side columns, penalize */
        if (player_side_three_threat(&child, child.player_to_move, NULL)) {
            s -= 1200;
        }
        /* If this move prevents opponent immediate win, boost modestly */
        Position opp_view_child = child;
        flip_view(&opp_view_child); /* flip POV */
        int opp_can_win = 0;
        for (int oc = 0; oc < COLS; oc++) {
            if (!can_play(&opp_view_child, oc)) continue;
            Position t = opp_view_child;
            make_move(&t, oc);
            if (has_won_bb(opponent_bb(&t))) { opp_can_win = 1; break; }
        }
        if (!opp_can_win) s += 200;         /* urgent non-losing move */

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

/* Detect if opponent has a dangerous side-column vertical threat (3-stack)
   AND our move allows a horizontal fork around rows 2-4. */
static int creates_side_fork(Position *p, int my_move_col) {
    Position after = *p;
    make_move(&after, my_move_col);

    unsigned long long opp = after.bb[after.player_to_move]; /* opponent POV */
    unsigned long long occ = mask_all(&after);

    /* Check side columns 0,1,5,6 */
    int sides[4] = {0,1,5,6};
    for (int i = 0; i < 4; i++) {
        int c = sides[i];
        int h = column_height(occ, c);

        /* If opponent already has exactly 3 stones stacked */
        if (h >= 3) {
            int r0 = c * 7 + 0;
            int r1 = c * 7 + 1;
            int r2 = c * 7 + 2;

            if ((opp & (1ULL << r0)) &&
                (opp & (1ULL << r1)) &&
                (opp & (1ULL << r2))) {

                /* Now scan rows 1–3 for horizontal fork possibilities */
                for (int cc = 0; cc < COLS - 3; cc++) {
                    for (int row = 1; row <= 3; row++) {
                        int opp_count = 0, empty_count = 0;

                        for (int k = 0; k < 4; k++) {
                            unsigned long long bit =
                                1ULL << ((cc + k) * 7 + row);
                            if (opp & bit) opp_count++;
                            else if (!(occ & bit)) empty_count++;
                        }

                        if (opp_count == 2 && empty_count == 2)
                            return 1;   /* This move allows a fork */
                    }
                }
            }
        }
    }

    return 0;
}

/* ==================== public entry ==================== */

int getBotMoveHard(char board[ROWS][COLS], char bot, char opponent) {
    (void)opponent; /* board contents + bot char define everything */
    init_zobrist();

    /* Build Position bitboards + zobrist */
    init_tt();
    for (int i = 0; i < MAX_PLY; i++) {
        killer_moves[0][i] = -1;
        killer_moves[1][i] = -1;
    }
    for (int i = 0; i < 7; i++) history_table[i] = 0;
    Position pos;
    pos.bb[0] = pos.bb[1] = 0;
    pos.zkey = 0;
    pos.player_to_move = 0; /* this function is called only when the bot is to move */
    for (int c = 0; c < COLS; c++) {
        for (int r = 0; r < ROWS; r++) {
            char cell = board[r][c];
            if (cell == '.') continue;
            int row_from_bottom = ROWS - 1 - r;
            int idx = c * 7 + row_from_bottom;
            unsigned long long bit = 1ULL << idx;
            int owner = (cell == bot) ? 0 : 1;
            pos.bb[owner] |= bit;
            if (row_from_bottom < 6) {
                pos.zkey ^= zobrist[c][row_from_bottom][owner];
            }
        }
    }

    /* Advance TT generation to age out old entries without a full clear */
    tt_generation++;
    if (tt_generation == 0) tt_generation = 1; /* avoid 0 to keep "unused" distinct */
    vcf_generation++;
    if (vcf_generation == 0) vcf_generation = 1;

    clock_t start_time = clock();

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
        flip_view(&tmp);
        make_move(&tmp, c);
        if (has_won_bb(opponent_bb(&tmp))) {
            return c + 1;
        }
    }

    /* Emergency block: prevent opponent from stacking to 3 in side columns */
    int danger_col = -1;
    if (player_side_three_threat(&pos, 1, &danger_col) && can_play(&pos, danger_col)) {
        /* try to block unless it immediately loses */
        Position tmp = pos;
        make_move(&tmp, danger_col);
        Position opp_check = tmp;
        flip_view(&opp_check);
        if (!has_won_bb(opponent_bb(&opp_check))) {
            return danger_col + 1;
        }
    }

    /* Late-game threat-space search to catch forced wins/defenses quickly (hint only). */
    int tss_hint = -1;
    int empties = empties_on_board(&pos);
    if (empties <= 24) {
        int tss_depth = empties < VCF_MAX_DEPTH ? empties : VCF_MAX_DEPTH;
        int tss_move = threat_space_root(&pos, tss_depth, start_time);
        if (tss_move >= 1 && board[0][tss_move - 1] == '.') {
            tss_hint = tss_move - 1; /* store as 0-based to prefer in ordering */
        }
    }

    /* iterative deepening with time limit + aspiration windows */
    int depth_cap = empties;        /* theoretical maximum remaining ply */

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
        /* Prefer threat-space hint if present */
        if (tss_hint >= 0) {
            for (int i = 0; i < mcount; i++) {
                if (moves[i] == tss_hint) {
                    int tmp = moves[0];
                    moves[0] = moves[i];
                    moves[i] = tmp;
                    break;
                }
            }
        }

        
      

        /* Discard moves that hand opponent an immediate double threat, if we have safer options */
        int safe_moves[7];
        int safe_count = 0;
        for (int i = 0; i < mcount; i++) {
            int c = moves[i];
            Position child = pos;
            make_move(&child, c);
            Position opp_view = child;
            flip_view(&opp_view);
            int opp_wins = count_immediate_wins(&opp_view);
          if (opp_wins >= 2) continue;
if (creates_side_fork(&pos, c)) continue;

            safe_moves[safe_count++] = c;
        }
        if (safe_count > 0) {
            for (int i = 0; i < safe_count; i++) moves[i] = safe_moves[i];
            mcount = safe_count;
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

    /* Avoid picking a move that hands the opponent an immediate win or double threat */
    if (best_move >= 1 && best_move <= COLS && board[0][best_move - 1] == '.') {
        Position tmp = pos;
        make_move(&tmp, best_move - 1); /* bot makes the move, POV flips to opponent */
        Position opp_view = tmp;
        flip_view(&opp_view); /* ensure we count wins from opponent POV */
        int opp_wins = count_immediate_wins(&opp_view);
        if (opp_wins > 0) {
            /* try to find a fallback that avoids immediate loss */
            int safe_move = -1;
            int safe_score = -WIN_SCORE;
            for (int c = 0; c < COLS; c++) {
                if (!can_play(&pos, c)) continue;
                Position cand = pos;
                make_move(&cand, c);
                Position opp_after_view = cand;
                flip_view(&opp_after_view);
                int opp_after = count_immediate_wins(&opp_after_view);
                if (opp_after > 1) continue; /* still leaves double threat */
                if (opp_after > 0) continue; /* still losing immediately */
                int s = evaluate_position_internal(&cand, 0);
                if (s > safe_score) { safe_score = s; safe_move = c; }
            }
            if (safe_move != -1) best_move = safe_move + 1;
        }
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
