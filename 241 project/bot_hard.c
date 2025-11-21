#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdio.h>
#include "bot_hard.h"

/* Bitboard-based solver with alpha-beta + transposition table + iterative deepening + time budget.
 * Default: DEPTH_LIMIT = 0 (no artificial cap).
 * If time expires, returns best move from last completed depth.
 */

#define TT_BITS        22              /* 4M entries ~64MB; adjust if needed */
#define TT_SIZE        (1U << TT_BITS)
#define TT_MASK        (TT_SIZE - 1)
#define WIN_SCORE      1000000
#define TIME_LIMIT_SEC 5.5

typedef struct {
    unsigned long long key;
    int depth;      /* remaining depth searched */
    int value;      /* score from side-to-move POV at that node */
    int move;       /* best move (1-based column index) */
    char flag;      /* 0 exact, 1 lowerbound, 2 upperbound */
} TTEntry;

static TTEntry ttable[TT_SIZE];

/* Bitboard layout: 7 bits per column, bottom at row 0.
 * Bit index = col * 7 + row.
 * Only rows 0..5 are actual board cells; row 6 is a sentinel.
 */

static inline unsigned long long bottom_mask(int col) { return 1ULL << (col * 7); }
static inline unsigned long long top_mask(int col)    { return 1ULL << (col * 7 + 5); }
static inline unsigned long long column_mask(int col) { return 0x7FULL << (col * 7); }

typedef struct {
    unsigned long long player;   /* stones of side to move */
    unsigned long long opponent; /* stones of the other side */
} Position;

/* Global depth cap (optional); 0 => no artificial cap */
static int DEPTH_LIMIT = 0;
void setHardBotDepthLimit(int ply) { if (ply >= 0) DEPTH_LIMIT = ply; }

/* Win detection on a single bitboard */
static inline int has_won(unsigned long long bb) {
    unsigned long long m;

    /* Vertical (shift by 1 row) */
    m = bb & (bb >> 1);
    if (m & (m >> 2)) return 1;

    /* Horizontal (shift by 1 col = 7 bits) */
    m = bb & (bb >> 7);
    if (m & (m >> 14)) return 1;

    /* Diagonal / (up-right: +8) */
    m = bb & (bb >> 8);
    if (m & (m >> 16)) return 1;

    /* Diagonal \ (up-left: +6) */
    m = bb & (bb >> 6);
    if (m & (m >> 12)) return 1;

    return 0;
}

/* =============== Zobrist =============== */

static unsigned long long zobrist[7][6][2];  /* [col][row][0=player,1=opponent] */
static int zobrist_ready = 0;

static unsigned long long splitmix64(unsigned long long *x) {
    unsigned long long z = (*x += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

static void init_zobrist(void) {
    if (zobrist_ready) return;
    zobrist_ready = 1;

    unsigned long long seed = 88172645463325252ULL;
    for (int c = 0; c < 7; c++) {
        for (int r = 0; r < 6; r++) {
            zobrist[c][r][0] = splitmix64(&seed);
            zobrist[c][r][1] = splitmix64(&seed);
        }
    }
}

static inline unsigned long long compute_key(const Position *p) {
    unsigned long long key = 0;
    unsigned long long player = p->player;
    unsigned long long opp    = p->opponent;

    for (int c = 0; c < 7; c++) {
        for (int r = 0; r < 6; r++) {
            unsigned long long bit = 1ULL << (c * 7 + r);
            if (player & bit)      key ^= zobrist[c][r][0];
            else if (opp & bit)    key ^= zobrist[c][r][1];
        }
    }
    return key;
}

static inline int tt_idx(unsigned long long key) {
    return (int)(key & TT_MASK);
}

/* =============== Position helpers =============== */

static inline unsigned long long occ(const Position *p) {
    return p->player | p->opponent;
}

static inline int is_legal_col(const Position *p, int col) {
    return (occ(p) & top_mask(col)) == 0;
}

/* True if no legal moves left. */
static inline int board_full_pos(const Position *p) {
    for (int c = 0; c < 7; c++) {
        if (is_legal_col(p, c)) return 0;
    }
    return 1;
}

/* Create child position after side-to-move plays in column col.
 * Negamax convention: in the child, "player" is the next side to move,
 * and "opponent" contains the stones of the side that just moved.
 */
static inline void make_child(const Position *p, int col, Position *child) {
    unsigned long long o = occ(p);
    unsigned long long move = (o + bottom_mask(col)) & column_mask(col);
    child->player   = p->opponent;
    child->opponent = p->player | move;
}

/* Move ordering (center-first) */
static const int ORDER[7] = {3, 2, 4, 1, 5, 0, 6};
static void gen_moves(const Position *p, int moves[7], int *count) {
    *count = 0;
    for (int i = 0; i < 7; i++) {
        int c = ORDER[i];
        if (is_legal_col(p, c)) {
            moves[(*count)++] = c;
        }
    }
}

/* =============== Timing =============== */

static inline int timed_out(clock_t start_time) {
    return ((double)(clock() - start_time) / (double)CLOCKS_PER_SEC) >= TIME_LIMIT_SEC;
}

/* =============== Negamax with alpha-beta + TT + time =================
 *
 * Position p is always from the POV of side-to-move.
 * If the previous move created a 4-in-a-row for the opponent, that is
 * detected via has_won(p->opponent) and scored as a loss.
 */


static int negamax(const Position *p,
                   int depth_rem, int max_depth,
                   int alpha, int beta,
                   unsigned long long key,
                   int *best_move_out,
                   clock_t start_time,
                   int *timeout_flag)
{
    int alpha0 = alpha;
    int beta0  = beta;

    /* Global time check */
    if (timeout_flag && *timeout_flag) return 0;
    if (timed_out(start_time)) {
        if (timeout_flag) *timeout_flag = 1;
        return 0;
    }

    /* Terminal: opponent just played and has 4 in a row */
    if (has_won(p->opponent)) {
        return -WIN_SCORE + (max_depth - depth_rem);
    }

    /* Draw or depth cutoff */
    if (depth_rem == 0 || board_full_pos(p) ||
        (DEPTH_LIMIT > 0 && (max_depth - depth_rem) >= DEPTH_LIMIT))
    {
        /* You can plug in a heuristic eval here instead of 0 if desired. */
        return 0;
    }

    /* Transposition table lookup */
    TTEntry *e = &ttable[tt_idx(key)];
    if (e->key == key && e->depth >= depth_rem) {
        if (e->flag == 0) {
            if (best_move_out) *best_move_out = e->move;
            return e->value;
        } else if (e->flag == 1 && e->value > alpha) {
            alpha = e->value; /* lower bound */
        } else if (e->flag == 2 && e->value < beta) {
            beta = e->value;  /* upper bound */
        }
        if (alpha >= beta) {
            if (best_move_out) *best_move_out = e->move;
            return e->value;
        }
    }

    int moves[7], mcount = 0;
    gen_moves(p, moves, &mcount);
    if (mcount == 0) return 0;

    int best_move_local = moves[0] + 1;
    int value = -WIN_SCORE;

    for (int i = 0; i < mcount; i++) {
        int c = moves[i];
        Position child;
        make_child(p, c, &child);

        unsigned long long child_key = compute_key(&child);

        int score = -negamax(&child, depth_rem - 1, max_depth,
                             -beta, -alpha, child_key, NULL,
                             start_time, timeout_flag);

        if (timeout_flag && *timeout_flag) return 0;

        if (score > value) {
            value = score;
            best_move_local = c + 1; /* back to 1-based column index */
        }
        if (value > alpha) alpha = value;
        if (alpha >= beta) break;
    }

    /* Store in TT */
    e->key   = key;
    e->depth = depth_rem;
    e->value = value;
    e->move  = best_move_local;
    if (value <= alpha0)      e->flag = 2; /* upper bound */
    else if (value >= beta0) e->flag = 1; /* lower bound */
    else                     e->flag = 0; /* exact */

    if (best_move_out) *best_move_out = best_move_local;
    return value;
}

/* =================== Public bot entry =================== */

int getBotMoveHard(char board[ROWS][COLS], char bot, char opponent) {
    (void)opponent; /* board tells us where opponent is; param kept for API */
    init_zobrist();

    /* Build bitboards for current side (bot) and opponent */
    Position pos;
    pos.player = 0;
    pos.opponent = 0;

    for (int c = 0; c < COLS; c++) {
        for (int r = 0; r < ROWS; r++) {
            char cell = board[r][c];
            if (cell == '.') continue;
            int row_from_bottom = ROWS - 1 - r;    /* bottom row index = 0 */
            int idx = c * 7 + row_from_bottom;
            if (cell == bot)         pos.player   |= 1ULL << idx;
            else /* opponent */      pos.opponent |= 1ULL << idx;
        }
    }

    /* Immediate win for bot (side to move) */
    for (int c = 0; c < COLS; c++) {
        if (!is_legal_col(&pos, c)) continue;
        unsigned long long o = occ(&pos);
        unsigned long long move = (o + bottom_mask(c)) & column_mask(c);
        unsigned long long new_player = pos.player | move;
        if (has_won(new_player)) {
            return c + 1; /* 1-based column index */
        }
    }

    /* Immediate block: if opponent can win next move, block it */
    for (int c = 0; c < COLS; c++) {
        if (!is_legal_col(&pos, c)) continue;
        unsigned long long o = occ(&pos);
        unsigned long long move = (o + bottom_mask(c)) & column_mask(c);
        unsigned long long new_opp = pos.opponent | move;
        if (has_won(new_opp)) {
            return c + 1;
        }
    }

    /* Iterative deepening search with time limit */
    unsigned long long key = compute_key(&pos);
    int best_move = 4;  /* prefer center by default */
    clock_t start_time = clock();
    double elapsed = 0.0;
    int timeout_flag = 0;
    int last_completed_depth = 0;

    /* Depth cap (optional): can't exceed empties, and optional DEPTH_LIMIT */
    int empties = 0;
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++)
            if (board[r][c] == '.') empties++;

    int depth_cap = empties;
    if (DEPTH_LIMIT > 0 && DEPTH_LIMIT < depth_cap) depth_cap = DEPTH_LIMIT;

    for (int depth = 1; depth <= depth_cap; depth++) {
        int move_at_depth = best_move;
        timeout_flag = 0;

        (void)negamax(&pos, depth, depth,
                      -WIN_SCORE, WIN_SCORE,
                      key, &move_at_depth,
                      start_time, &timeout_flag);

        if (timeout_flag) break;  /* Use result from last completed depth */

        best_move = move_at_depth;
        last_completed_depth = depth;
    }

    /* Safety fallback: choose first legal column if best_move invalid */
    if (best_move < 1 || best_move > COLS || board[0][best_move - 1] != '.') {
        for (int c = 1; c <= COLS; c++) {
            if (board[0][c - 1] == '.') {
                best_move = c;
                break;
            }
        }
    }

    elapsed = (double)(clock() - start_time) / (double)CLOCKS_PER_SEC;
    printf("[Hard bot] Depth reached: %d, Move time: %.3f s\n", last_completed_depth, elapsed);

    return best_move;
}
