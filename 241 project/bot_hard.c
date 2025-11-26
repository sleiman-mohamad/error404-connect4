#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include "bot_hard.h"
#define PASCAL_BOOK_FILE "7x6.book"

#define USE_THREADS 1         /* set to 0 if you can't use pthreads */

#if USE_THREADS
#include <pthread.h>
#endif

/* ===============================================================
   Strong Connect-4 bot (bitboards, multithreaded) + Pascal book
   ---------------------------------------------------------------
   - Bitboard representation: (position, mask), 7 bits/column
   - Root opening book from Pascal Pons 7x6.book
       * Real binary format with header + key/value arrays
       * Uses Pascal's symmetric base-3 key3() over (position, mask)
       * Book depth from file header (for 7x6.book: depth = 14)
   - Alpha-beta + transposition table with bounds
   - Iterative deepening with ~10s time limit
   - TT-based move ordering + center-first fallback
   - Aspiration windows at root
   - Late move reduction (LMR) inside negamax
   - Root-level multithreading: one thread per playable column
   =============================================================== */

#define WIN_SCORE        1000000
#define LOSS_SCORE      -1000000
#define INF_SCORE        2000000000

/* Safety margin under 10s */
#define TIME_LIMIT_SEC   9.8

/* Transposition table: 2^22 ~ 4M entries (~64MB).
 * If that's too big on your machine, drop back to 21. */
#define TT_BITS   22
#define TT_SIZE   (1u << TT_BITS)
#define TT_MASK   (TT_SIZE - 1)

/* LMR settings (very standard, quite safe) */
#define LMR_MIN_DEPTH    5   /* only reduce when depth >= this */
#define LMR_MOVE_INDEX   3   /* reduce from 4th move onwards */
#define LMR_REDUCTION    1   /* reduce by 1 ply */

/* Threads: at most 7 columns anyway */
#if USE_THREADS
#define NUM_THREADS 7
#else
#define NUM_THREADS 1
#endif

/* -------- Pascal book config (binary file) -------- */
#define PASCAL_BOOK_FILE "7x6.book"  /* must be in same directory as executable */
#define PASCAL_WIDTH   7
#define PASCAL_HEIGHT  6
#define PASCAL_MIN_SCORE (-(PASCAL_WIDTH * PASCAL_HEIGHT) / 2 + 3) /* = -18 on 7x6 */

// Globals to track load    
static int pascalBookLoaded = 0;

// Forward declarations
static void pascal_book_load(const char *filename);



void initHardBot();

/* ===============================================================
   Your engine position + TT
   =============================================================== */

typedef struct {
    uint64_t position;   /* stones of player to move in this node */
    uint64_t mask;       /* stones of both players                */
    int      moves;      /* number of stones on board             */
} Position;

typedef struct {
    uint64_t key;
    int32_t  value;
    int16_t  depth;
    int8_t   flag;       /* 0 exact, 1 lower bound, 2 upper bound */
    int8_t   bestMove;   /* 0..6 or -1                            */
} TTEntry;

static TTEntry tt[TT_SIZE];

/* precomputed masks for each column */
static uint64_t bottomMask[COLS];
static uint64_t columnMask[COLS];
static uint64_t topMask[COLS];

/* center-first base move ordering (fallback when no TT hint) */
static const int moveOrder[COLS] = {3, 2, 4, 1, 5, 0, 6};

/* timing globals */
static clock_t startTime;
static volatile int timeExpired = 0;
static int lastCompletedDepth = 0;
static int lastSelectiveDepth = 0;

/* ===============================================================
   Pascal-compatible position + opening book structures
   =============================================================== */

typedef struct {
    uint64_t current_position; /* stones of player to move */
    uint64_t mask;             /* stones of both players   */
    unsigned int moves;        /* number of stones         */
} PascalPos;

typedef struct {
    uint8_t *keys;    /* partial keys array (size * partial_key_bytes) */
    uint8_t *values;  /* value bytes (size * 1)                         */
    size_t   size;    /* number of stored elements (prime, ~2^24)       */
    int      partial_key_bytes; /* 1 in Pascal 7x6.book                  */
    int      depth;             /* max nbMoves stored                    */
    int      loaded;            /* 0 = not tried, 1 = attempted          */
    int      ok;                /* 1 if successfully loaded              */
} PascalBook;

static PascalBook g_book = {0};

/* ---------------------------------------------------------------
   Pascal key3() implementation for 7x6 (symmetric base-3 key)
   ------------------------------------------------------------ */

static inline void pascal_partialKey3(uint64_t *key, const PascalPos *P, int col) {
    uint64_t mask = P->mask;
    uint64_t cur  = P->current_position;
    uint64_t pos;

    for (pos = 1ULL << (col * (PASCAL_HEIGHT + 1)); pos & mask; pos <<= 1) {
        *key *= 3ULL;
        if (pos & cur) *key += 1ULL;
        else           *key += 2ULL;
    }
    *key *= 3ULL;
}

static uint64_t pascal_key3(const PascalPos *P) {
    uint64_t key_forward = 0;
    uint64_t key_reverse = 0;

    /* forward columns 0..6 */
    for (int i = 0; i < PASCAL_WIDTH; ++i) {
        pascal_partialKey3(&key_forward, P, i);
    }

    /* reverse columns 6..0 */
    for (int i = PASCAL_WIDTH - 1; i >= 0; --i) {
        pascal_partialKey3(&key_reverse, P, i);
    }

    /* smallest of forward/reverse, divide by 3 as last trit always 0 */
    if (key_forward < key_reverse)
        return key_forward / 3ULL;
    else
        return key_reverse / 3ULL;
}

/* ---------------------------------------------------------------
   Load Pascal 7x6.book (binary) into memory
   ------------------------------------------------------------ */

static void pascal_book_load(const char *filename) {
    if (g_book.loaded) return;      /* only try once */
    g_book.loaded = 1;

    FILE *f = fopen(filename, "rb");
    if (!f) {
        fprintf(stderr, "[HARD BOT] opening book '%s' not found, continuing without book.\n",
                filename);
        g_book.ok = 0;
        return;
    }

    unsigned char header[6];
    if (fread(header, 1, 6, f) != 6) {
        fprintf(stderr, "[HARD BOT] failed to read header from '%s'.\n", filename);
        fclose(f);
        g_book.ok = 0;
        return;
    }

    int width   = header[0];
    int height  = header[1];
    int depth   = header[2];
    int keyBytes   = header[3];
    int valueBytes = header[4];
    int log_size   = header[5]; /* unused, but kept for completeness */

    if (width != PASCAL_WIDTH || height != PASCAL_HEIGHT) {
        fprintf(stderr,
                "[HARD BOT] book '%s' has wrong board size (got %d x %d, expected %d x %d).\n",
                filename, width, height, PASCAL_WIDTH, PASCAL_HEIGHT);
        fclose(f);
        g_book.ok = 0;
        return;
    }
    if (valueBytes != 1) {
        fprintf(stderr,
                "[HARD BOT] book '%s' has unsupported value size %d (expected 1).\n",
                filename, valueBytes);
        fclose(f);
        g_book.ok = 0;
        return;
    }
    if (keyBytes < 1 || keyBytes > 8) {
        fprintf(stderr,
                "[HARD BOT] book '%s' has invalid key size %d.\n",
                filename, keyBytes);
        fclose(f);
        g_book.ok = 0;
        return;
    }

    /* compute table size from file size */
    if (fseek(f, 0, SEEK_END) != 0) {
        fprintf(stderr, "[HARD BOT] fseek failed on '%s'.\n", filename);
        fclose(f);
        g_book.ok = 0;
        return;
    }
    long fsize = ftell(f);
    if (fsize < 0) {
        fprintf(stderr, "[HARD BOT] ftell failed on '%s'.\n", filename);
        fclose(f);
        g_book.ok = 0;
        return;
    }
    long payload = fsize - 6L;
    long perEntry = keyBytes + valueBytes;
    if (payload <= 0 || payload % perEntry != 0) {
        fprintf(stderr,
                "[HARD BOT] book '%s' payload size mismatch (payload=%ld, perEntry=%ld).\n",
                filename, payload, perEntry);
        fclose(f);
        g_book.ok = 0;
        return;
    }
    size_t size = (size_t)(payload / perEntry);

    if (fseek(f, 6L, SEEK_SET) != 0) {
        fprintf(stderr, "[HARD BOT] fseek data failed on '%s'.\n", filename);
        fclose(f);
        g_book.ok = 0;
        return;
    }

    uint8_t *keys = (uint8_t*)malloc(size * keyBytes);
    uint8_t *vals = (uint8_t*)malloc(size * valueBytes);
    if (!keys || !vals) {
        fprintf(stderr, "[HARD BOT] memory alloc failed for book '%s'.\n", filename);
        free(keys);
        free(vals);
        fclose(f);
        g_book.ok = 0;
        return;
    }

    if (fread(keys, keyBytes, size, f) != size ||
        fread(vals, valueBytes, size, f) != size) {
        fprintf(stderr, "[HARD BOT] failed to read key/value arrays from '%s'.\n",
                filename);
        free(keys);
        free(vals);
        fclose(f);
        g_book.ok = 0;
        return;
    }

    fclose(f);

    g_book.keys = keys;
    g_book.values = vals;
    g_book.size = size;
    g_book.partial_key_bytes = keyBytes;
    g_book.depth = depth;
    g_book.ok = 1;

    fprintf(stderr,
            "[HARD BOT] Pascal 7x6.book loaded: size=%zu, depth=%d, keyBytes=%d, log_size=%d\n",
            g_book.size, g_book.depth, g_book.partial_key_bytes, log_size);
}
void initHardBot() {
    /* nothing to initialize here yet */
}


/* ---------------------------------------------------------------
   Query Pascal book for a position (score for player to move)
   Returns 1 if found, 0 if not. Score is in [-18,18] for 7x6.
   ------------------------------------------------------------ */

static int pascal_book_score(const PascalPos *P, int *outScore) {
    if (!g_book.ok) return 0;
    if ((int)P->moves > g_book.depth) return 0;

    uint64_t key3 = pascal_key3(P);
    size_t idx = (size_t)(key3 % g_book.size);

    int kb = g_book.partial_key_bytes;
    const uint8_t *kptr = g_book.keys + (size_t)kb * idx;

    uint64_t stored = 0;
    for (int i = 0; i < kb; ++i) {
        stored |= ((uint64_t)kptr[i]) << (8 * i); /* little endian */
    }

    uint64_t partialMask = (kb >= 8) ? ~0ULL : ((1ULL << (8 * kb)) - 1ULL);
    uint64_t expected = key3 & partialMask;

    if (stored != expected) {
        return 0; /* collision or empty */
    }

    uint8_t raw = g_book.values[idx];
    if (raw == 0) return 0; /* "missing data" in Pascal's code */

    /* Decode as in Solver::negamax: score = val + MIN_SCORE - 1 */
    int score = (int)raw + PASCAL_MIN_SCORE - 1;
    *outScore = score;
    return 1;
}

/* ---------------------------------------------------------------
   Try to get a perfect root move from Pascal book.
   Returns 1 if book fully covers all legal moves and outMove is set.
   Otherwise returns 0 and engine should run normal search.
   ------------------------------------------------------------ */

static int try_opening_book(const Position *root, int *outMove) {
    pascal_book_load(PASCAL_BOOK_FILE);
    if (!g_book.ok) return 0;

    PascalPos rootP;
    rootP.current_position = root->position;
    rootP.mask             = root->mask;
    rootP.moves            = (unsigned int)root->moves;

    if ((int)rootP.moves > g_book.depth) {
        return 0; /* outside book depth */
    }

    int playableCount = 0;
    for (int c = 0; c < COLS; ++c)
        if ((root->mask & topMask[c]) == 0ULL)
            playableCount++;

    if (playableCount == 0) return 0;

    int bestCol    = -1;
    int bestScore  = -1000000;
    int allCovered = 1;

    for (int c = 0; c < COLS; ++c) {
        if ((root->mask & topMask[c]) != 0ULL) continue;

        Position child = *root;
        /* use our own play_move to advance bitboards */
        {
            uint64_t m = child.mask;
            uint64_t move = (m + bottomMask[c]) & columnMask[c];
            child.position ^= m;
            child.mask = m | move;
            child.moves++;
        }

        PascalPos childP;
        childP.current_position = child.position;
        childP.mask             = child.mask;
        childP.moves            = (unsigned int)child.moves;

        int val;
        if (!pascal_book_score(&childP, &val)) {
            allCovered = 0;
            continue;
        }

        /* child score is for side to move in child (opponent), so negate */
        int rootScore = -val;

        if (bestCol == -1 || rootScore > bestScore) {
            bestScore = rootScore;
            bestCol   = c;
        }
    }

    if (bestCol != -1 && allCovered) {
        *outMove = bestCol;
        return 1;
    }

    return 0;
}

/* ===============================================================
   Bitboard helpers (your engine)
   =============================================================== */

static void init_masks(void) {
    for (int c = 0; c < COLS; ++c) {
        bottomMask[c] = 1ULL << (c * 7);
        columnMask[c] = ((1ULL << ROWS) - 1ULL) << (c * 7);
        topMask[c]    = 1ULL << (c * 7 + (ROWS - 1));
    }
}

static inline int time_up(void) {
    double elapsed = (double)(clock() - startTime) / CLOCKS_PER_SEC;
    return elapsed >= TIME_LIMIT_SEC;
}

/* opponent stones = mask XOR position */
static inline uint64_t opponent_bb(const Position *p) {
    return p->mask ^ p->position;
}

/* can we still play in this column? */
static inline int can_play(const Position *p, int col) {
    return (p->mask & topMask[col]) == 0ULL;
}

/* play a move in column col, using Pascal-style position/mask flip */
static inline void play_move(Position *p, int col) {
    uint64_t m = p->mask;
    uint64_t move = (m + bottomMask[col]) & columnMask[col];
    p->position ^= m;
    p->mask = m | move;
    p->moves++;
}

/* detect a connect-4 in bitboard bb */
static int has_connect4(uint64_t bb) {
    uint64_t m;

    /* horizontal (shift 7) */
    m = bb & (bb >> 7);
    if (m & (m >> 14)) return 1;

    /* diagonal \ (shift 6) */
    m = bb & (bb >> 6);
    if (m & (m >> 12)) return 1;

    /* diagonal / (shift 8) */
    m = bb & (bb >> 8);
    if (m & (m >> 16)) return 1;

    /* vertical (shift 1) */
    m = bb & (bb >> 1);
    if (m & (m >> 2)) return 1;

    return 0;
}

/* count 2- and 3-in-a-row patterns in all directions for a given bitboard */
static int pattern_score(uint64_t b) {
    int s = 0;
    uint64_t m;

    /* horizontal (shift 7) */
    m = b & (b >> 7);                     /* at least 2 in a row */
    s += __builtin_popcountll(m) * 2;
    m &= (b >> 14);                       /* now 3 in a row */
    s += __builtin_popcountll(m) * 5;

    /* vertical (shift 1) */
    m = b & (b >> 1);
    s += __builtin_popcountll(m) * 2;
    m &= (b >> 2);
    s += __builtin_popcountll(m) * 5;

    /* diagonal / (shift 6) */
    m = b & (b >> 6);
    s += __builtin_popcountll(m) * 2;
    m &= (b >> 12);
    s += __builtin_popcountll(m) * 5;

    /* diagonal \ (shift 8) */
    m = b & (b >> 8);
    s += __builtin_popcountll(m) * 2;
    m &= (b >> 16);
    s += __builtin_popcountll(m) * 5;

    return s;
}

/* improved evaluation: center + patterns + small tempo bias */
static int evaluate(const Position *p) {
    uint64_t cur = p->position;
    uint64_t opp = opponent_bb(p);

    /* center control */
    uint64_t center = columnMask[3];
    int centerScore = (int)__builtin_popcountll(cur & center)
                    - (int)__builtin_popcountll(opp & center);

    int score = centerScore * 6;  /* slightly stronger weight */

    /* pattern-based score (2- and 3-in-a-row) */
    int curPatterns = pattern_score(cur);
    int oppPatterns = pattern_score(opp);
    score += (curPatterns - oppPatterns);

    /* early-game anti-overstack in center:
       if both players are contesting the center and we already have
       2 or more stones there, slightly discourage adding even more.
       This mainly affects the first few plies and helps the bot,
       especially as second player, to explore side threats instead
       of blindly building a tall center pillar. */
    if (p->moves <= 8) {
        int myCenter = __builtin_popcountll(cur & center);
        int oppCenter = __builtin_popcountll(opp & center);
        if (myCenter >= 2 && oppCenter >= 2) {
            score -= (myCenter - 1) * 20;
        }
    }

    /* tiny tempo bias */
    score += (p->moves - 21);

    return score;
}

/* ===============================================================
   Transposition table (partitioned per thread)
   =============================================================== */

/* partition TT among threads by reserving low bits for thread id */
static inline unsigned tt_index(uint64_t key, int thread_id) {
#if USE_THREADS
    unsigned idx = (unsigned)(key & TT_MASK);
    /* reserve low bits for thread id (NUM_THREADS <= 8) */
    unsigned lowMask = NUM_THREADS - 1;
    idx = (idx & ~lowMask) | (unsigned)thread_id;
    return idx;
#else
    (void)thread_id;
    return (unsigned)(key & TT_MASK);
#endif
}

static inline uint64_t hash_position(const Position *p) {
    uint64_t x = p->position * 0x9E3779B185EBCA87ULL;
    uint64_t y = p->mask      * 0xC2B2AE3D27D4EB4FULL;
    x ^= y >> 23;
    y ^= x << 17;
    return x ^ y;
}

/* TT probe now also returns bestMove hint (for move ordering) */
static int tt_probe(const Position *p, int depth, int alpha, int beta,
                    int *outVal, int *outBestMove, int thread_id) {
    uint64_t key = hash_position(p);
    TTEntry *e = &tt[tt_index(key, thread_id)];

    if (e->key != key || e->depth < depth) {
        return 0;
    }

    int v = e->value;
    if (outBestMove) {
        *outBestMove = e->bestMove;   /* can be -1..6 */
    }

    if (e->flag == 0) {
        *outVal = v;
        return 1;
    }
    if (e->flag == 1 && v > alpha) alpha = v;
    if (e->flag == 2 && v < beta)  beta  = v;
    if (alpha >= beta) {
        *outVal = v;
        return 1;
    }
    return 0;
}

static void tt_store(const Position *p, int depth, int value, int flag,
                     int bestMove, int thread_id) {
    uint64_t key = hash_position(p);
    TTEntry *e = &tt[tt_index(key, thread_id)];

    if (e->key == key && e->depth > depth) {
        return; /* keep deeper entry */
    }
    e->key      = key;
    e->depth    = (int16_t)depth;
    e->value    = value;
    e->flag     = (int8_t)flag;
    e->bestMove = (int8_t)bestMove;
}

/* ===============================================================
   Core negamax + alpha-beta + LMR (+ selective depth tracking)
   =============================================================== */

static int negamax(Position *p, int depth, int alpha, int beta,
                   int thread_id, int ply) {
    if (timeExpired || time_up()) {
        timeExpired = 1;
        return evaluate(p);
    }

    /* track deepest selective depth reached */
    if (ply > lastSelectiveDepth) {
        lastSelectiveDepth = ply;
    }

    int remaining = ROWS * COLS - p->moves;
    /* in very late endgame, don’t search deeper than remaining moves */
    if (remaining <= 8 && depth > remaining) {
        depth = remaining;
    }

    /* if previous player already made a connect-4, this is losing */
    uint64_t opp = opponent_bb(p);
    if (has_connect4(opp)) {
        return LOSS_SCORE + p->moves;
    }

    if (p->moves == ROWS * COLS) {
        return 0; /* draw */
    }

    if (depth == 0) {
        return evaluate(p);
    }

    int alphaOrig = alpha;
    int ttVal;
    int ttMove = -1;

    /* TT lookup: may give us a value AND a suggested bestMove for ordering */
    if (tt_probe(p, depth, alpha, beta, &ttVal, &ttMove, thread_id)) {
        return ttVal;
    }

    int bestVal  = -INF_SCORE;
    int bestMove = -1;

    /* ----- Build ordered move list for this node -----
       1) TT bestMove first (if valid & playable)
       2) Then remaining moves in center-first order
    */
    int ordered[COLS];
    int count = 0;

    if (ttMove >= 0 && ttMove < COLS && can_play(p, ttMove)) {
        ordered[count++] = ttMove;
    }

    for (int i = 0; i < COLS; ++i) {
        int col = moveOrder[i];
        if (col == ttMove) continue;      /* already added */
        if (!can_play(p, col)) continue;
        ordered[count++] = col;
    }

    /* Fallback: if still no moves collected, just scan all columns */
    if (count == 0) {
        for (int c = 0; c < COLS; ++c) {
            if (can_play(p, c)) {
                ordered[count++] = c;
            }
        }
    }

    int localAlpha = alpha;

    for (int i = 0; i < count; ++i) {
        int col = ordered[i];

        Position child = *p;
        play_move(&child, col);

        /* Check if this move immediately wins (for LMR safety) */
        uint64_t prevPlayerBB = opponent_bb(&child);  /* previous mover's stones */
        int immediateWin = has_connect4(prevPlayerBB);

        int newDepth = depth - 1;
        int val;

        /* LMR: for later moves at sufficient depth that are NOT immediate wins,
           try a reduced-depth null-window search first. */
        int doLMR = (newDepth >= LMR_MIN_DEPTH &&
                     i >= LMR_MOVE_INDEX &&
                     !immediateWin);

        if (doLMR) {
            int rDepth = newDepth - LMR_REDUCTION;
            if (rDepth < 1) rDepth = 1;

            /* Reduced-depth null-window search */
            val = -negamax(&child, rDepth,
                           -localAlpha - 1, -localAlpha,
                           thread_id, ply + 1);

            if (timeExpired) {
                return evaluate(p);
            }

            /* If it looks interesting, re-search with full depth/window */
            if (val > localAlpha) {
                val = -negamax(&child, newDepth,
                               -beta, -localAlpha,
                               thread_id, ply + 1);
                if (timeExpired) {
                    return evaluate(p);
                }
            }
        } else {
            /* Normal full-depth search */
            val = -negamax(&child, newDepth,
                           -beta, -localAlpha,
                           thread_id, ply + 1);
            if (timeExpired) {
                return evaluate(p);
            }
        }

        if (val > bestVal) {
            bestVal  = val;
            bestMove = col;
        }
        if (val > localAlpha) {
            localAlpha = val;
        }
        if (localAlpha >= beta) break; /* beta cut */
    }

    if (bestMove == -1) {
        return 0;
    }

    int flag;
    if (bestVal <= alphaOrig)      flag = 2; /* upper bound */
    else if (bestVal >= beta)      flag = 1; /* lower bound */
    else                           flag = 0; /* exact */

    tt_store(p, depth, bestVal, flag, bestMove, thread_id);
    return bestVal;
}

/* ===============================================================
   Convert from char board[ROWS][COLS] to bitboard Position
   =============================================================== */

static void load_board(Position *pos,
                       char board[ROWS][COLS],
                       char bot, char oppChar) {
    memset(pos, 0, sizeof(*pos));

    uint64_t pBot = 0ULL, pOpp = 0ULL, mask = 0ULL;
    int countBot = 0, countOpp = 0;

    /* We assume board[0][c] is top, board[ROWS-1][c] is bottom.
       Build each column bottom-up with no holes: break at first empty. */
    for (int c = 0; c < COLS; ++c) {
        int h = 0;
        for (int r = ROWS - 1; r >= 0; --r) {
            char cell = board[r][c];

            if (cell != bot && cell != oppChar) {
                /* treat anything else as empty */
                break;
            }

            uint64_t bit = 1ULL << (c * 7 + h);
            mask |= bit;
            if (cell == bot) {
                pBot |= bit;
                countBot++;
            } else if (cell == oppChar) {
                pOpp |= bit;
                countOpp++;
            }
            h++;
        }
    }

    pos->mask     = mask;
    pos->moves    = countBot + countOpp;
    pos->position = pBot;  /* root is always from BOT's POV (bot is to move) */
}

/* ===============================================================
   Root-level multithreading helper
   =============================================================== */

#if USE_THREADS

typedef struct {
    Position root;
    int depth;
    int col;
    int thread_id;
    int alpha;
    int beta;
    int score;
    int valid;
} ThreadTask;

static void *thread_search(void *arg) {
    ThreadTask *task = (ThreadTask *)arg;

    if (!can_play(&task->root, task->col)) {
        task->valid = 0;
        return NULL;
    }

    Position child = task->root;
    play_move(&child, task->col);

    int a = task->alpha;
    int b = task->beta;

    int val = -negamax(&child, task->depth - 1,
                       -b, -a,
                       task->thread_id, 1);

    if (timeExpired) {
        task->valid = 0;
        return NULL;
    }

    task->score = val;
    task->valid = 1;
    return NULL;
}

#endif

/* ===============================================================
   Root search with (alpha, beta) window
   =============================================================== */

static void root_search(Position *root,
                        int depth,
                        int alpha,
                        int beta,
                        int *outBestMove,
                        int *outBestScore) {
#if USE_THREADS
    pthread_t threads[NUM_THREADS];
    ThreadTask tasks[NUM_THREADS];
    int taskCount = 0;

    /* prepare tasks per playable column (still center-first at root) */
    for (int i = 0; i < COLS && taskCount < NUM_THREADS; ++i) {
        int col = moveOrder[i];
        if (!can_play(root, col)) continue;

        tasks[taskCount].root      = *root;
        tasks[taskCount].depth     = depth;
        tasks[taskCount].col       = col;
        tasks[taskCount].thread_id = taskCount;
        tasks[taskCount].alpha     = alpha;
        tasks[taskCount].beta      = beta;
        tasks[taskCount].score     = -INF_SCORE;
        tasks[taskCount].valid     = 0;

        pthread_create(&threads[taskCount], NULL, thread_search, &tasks[taskCount]);
        taskCount++;
    }

    /* wait for all searches to finish */
    for (int i = 0; i < taskCount; ++i) {
        pthread_join(threads[i], NULL);
    }

    if (timeExpired) {
        *outBestMove  = 3;
        *outBestScore = -INF_SCORE;
        return;
    }

    int localBestMove  = -1;
    int localBestScore = -INF_SCORE;

    for (int i = 0; i < taskCount; ++i) {
        if (!tasks[i].valid) continue;
        if (tasks[i].score > localBestScore) {
            localBestScore = tasks[i].score;
            localBestMove  = tasks[i].col;
        }
    }

    if (localBestMove == -1) {
        *outBestMove  = 3;
        *outBestScore = -INF_SCORE;
    } else {
        *outBestMove  = localBestMove;
        *outBestScore = localBestScore;
    }

#else
    /* single-threaded root */
    int localBestMove  = -1;
    int localBestScore = -INF_SCORE;
    int localAlpha = alpha;

    for (int i = 0; i < COLS; ++i) {
        int col = moveOrder[i];
        if (!can_play(root, col)) continue;

        Position child = *root;
        play_move(&child, col);

        int val = -negamax(&child, depth - 1,
                           -beta, -localAlpha,
                           0, 1);

        if (timeExpired) break;

        if (val > localBestScore) {
            localBestScore = val;
            localBestMove  = col;
        }
        if (val > localAlpha) {
            localAlpha = val;
        }
        if (localAlpha >= beta) break;
    }

    if (localBestMove == -1) {
        *outBestMove  = 3;
        *outBestScore = -INF_SCORE;
    } else {
        *outBestMove  = localBestMove;
        *outBestScore = localBestScore;
    }
#endif
}

/* ===============================================================
   Public entry point
   =============================================================== */

int getBotMoveHard(char board[ROWS][COLS], char bot, char opponent) {
      initHardBot();

   
    static int initialized = 0;
    if (!initialized) {
        init_masks();
        memset(tt, 0, sizeof(tt));
        initialized = 1;
    }

    /* Clear TT for each move to avoid cross-game pollution
       and make timing more predictable. */
    memset(tt, 0, sizeof(tt));

    Position root;
    load_board(&root, board, bot, opponent);

    /* ---- Try Pascal opening book first (perfect moves up to depth 14) ---- */
    int bookMove;
    if (try_opening_book(&root, &bookMove) && can_play(&root, bookMove)) {
        printf("[HARD BOT] opening book move=%d\n", bookMove + 1);
        return bookMove + 1;
    }

    startTime        = clock();
    timeExpired      = 0;
    lastCompletedDepth = 0;
    lastSelectiveDepth = 0;

    int bestMove  = 3;            /* default to center */
    int bestScore = -INF_SCORE;

    int maxDepth = ROWS * COLS - root.moves;
    if (maxDepth < 1) maxDepth = 1;

    int lastScore = 0;
    int haveLast  = 0;

    for (int depth = 1; depth <= maxDepth; ++depth) {
        if (timeExpired || time_up()) break;

        int alpha = -INF_SCORE;
        int beta  =  INF_SCORE;

        if (haveLast) {
            /* Aspiration window around lastScore */
            int window = 64;
            alpha = lastScore - window;
            beta  = lastScore + window;

            if (alpha < -INF_SCORE) alpha = -INF_SCORE;
            if (beta  >  INF_SCORE) beta  =  INF_SCORE;

            while (!timeExpired && !time_up()) {
                int localBestMove, localBestScore;
                root_search(&root, depth, alpha, beta,
                            &localBestMove, &localBestScore);

                if (timeExpired || time_up()) break;

                if (localBestScore <= alpha) {
                    /* fail-low: widen window downward */
                    alpha -= window;
                    if (alpha < -INF_SCORE) alpha = -INF_SCORE;
                    window *= 2;
                } else if (localBestScore >= beta) {
                    /* fail-high: widen window upward */
                    beta += window;
                    if (beta > INF_SCORE) beta = INF_SCORE;
                    window *= 2;
                } else {
                    /* inside window: accept */
                    bestMove  = localBestMove;
                    bestScore = localBestScore;
                    break;
                }
            }
        } else {
            /* First depth: full window */
            root_search(&root, depth, alpha, beta, &bestMove, &bestScore);
        }

        if (timeExpired || time_up()) break;

        lastScore = bestScore;
        haveLast  = 1;
        lastCompletedDepth = depth;

        /* found forced win; no need to go deeper */
        if (bestScore >= WIN_SCORE - 1000) {
            break;
        }
    }

    /* fallback: find any legal column if bestMove is invalid */
    if (!can_play(&root, bestMove)) {
        for (int c = 0; c < COLS; ++c) {
            if (can_play(&root, c)) {
                bestMove = c;
                break;
            }
        }
    }

    double elapsed = (double)(clock() - startTime) / CLOCKS_PER_SEC;
    printf("[HARD BOT] depth=%d  selective=%d  time=%.3f s  move=%d\n",
           lastCompletedDepth, lastSelectiveDepth, elapsed, bestMove + 1);

    return bestMove + 1;
}

