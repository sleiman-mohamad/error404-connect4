#include "rules.h"

static int in_bounds(int r, int c) {
    return (r >= 0 && r < ROWS && c >= 0 && c < COLS);
}

int place_checker(int board[ROWS][COLS], int col, Player p) {
    // col is 0-based; CLI will convert from user input 1..7 to 0..6
    if (col < 0 || col >= COLS) return -1;
    for (int r = ROWS - 1; r >= 0; r--) {
        if (board[r][col] == EMPTY) {
            board[r][col] = p;
            return r; // row where it landed
        }
    }
    return -1; // column full
}

static int count_line(int board[ROWS][COLS], int r, int c, int dr, int dc, Player p) {
    int cnt = 0;
    int rr = r, cc = c;
    while (in_bounds(rr, cc) && (Player)board[rr][cc] == p) {
        cnt++; rr += dr; cc += dc;
    }
    return cnt;
}

int check_win(int board[ROWS][COLS], Player p) {
    // Check from every cell owned by p, 4 directions: →, ↓, ↘, ↙
    const int dirs[4][2] = {{0,1},{1,0},{1,1},{1,-1}};
    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
            if ((Player)board[r][c] != p) continue;
            for (int d = 0; d < 4; d++) {
                int dr = dirs[d][0], dc = dirs[d][1];
                // Count both forward and backward while avoiding double-count of (r,c)
                int f = count_line(board, r, c, dr, dc, p);
                int b = count_line(board, r, c, -dr, -dc, p) - 1;
                if (f + b >= 4) return 1;
            }
        }
    }
    return 0;
}

int is_draw(int board[ROWS][COLS]) {
    // If any top cell is EMPTY, still playable
    for (int c = 0; c < COLS; c++) {
        if (board[0][c] == EMPTY) return 0;
    }
    return 1;
}
