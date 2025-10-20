#include <stdio.h>    // optional, but okay
#include <stdlib.h>   // for rand(), srand()
#include <time.h>     // for time()
#include "engine.h"
#define ROWS 6
#define COLS 7

// initialize the board with '.'
void init_board(char board[ROWS][COLS]) {
    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
            board[r][c] = '.';
        }
    }
}

// drop a piece into the column (1–7)
// return row index where placed, or -1 if full
int place_piece(char board[ROWS][COLS], int col, char player) {
    int c = col - 1; // convert to 0-based index
    if (c < 0 || c >= COLS) return -1;

    for (int r = ROWS - 1; r >= 0; r--) {
        if (board[r][c] == '.') { 
            board[r][c] = player;
            return r;
        }
    }
    return -1; // column full
}
#include <time.h>

int getBotMoveEasy(char board[ROWS][COLS]) {
    srand(time(NULL));
    int valid_cols[COLS];
    int count = 0;

    for (int c = 1; c <= COLS; c++) {
        if (board[0][c - 1] == '.') {
            valid_cols[count++] = c;
        }
    }

    if (count == 0) return 1; // fallback (board full)
    int random_index = rand() % count;
    return valid_cols[random_index];
}


// check in one direction how many matching pieces
static int count_dir(char board[ROWS][COLS], int r, int c, int dr, int dc) {
    char player = board[r][c];
    int count = 0;

    int rr = r + dr, cc = c + dc;
    while (rr >= 0 && rr < ROWS && cc >= 0 && cc < COLS &&
           board[rr][cc] == player) {
        count++;
        rr += dr;
        cc += dc;
    }
    return count;
}

// check winner after a move at (row,col)
int check_winner(char board[ROWS][COLS], int row, int col) {
    if (row < 0 || col < 0) return 0;
    char player = board[row][col];
    if (player == '.') return 0;

    // directions: horizontal, vertical, diag \, diag /
    int directions[4][2] = {
        {0, 1},  // horiz
        {1, 0},  // vert
        {1, 1},  // diag down-right
        {1, -1}  // diag down-left
    };

    for (int i = 0; i < 4; i++) {
        int dr = directions[i][0];
        int dc = directions[i][1];
        int count = 1;
        count += count_dir(board, row, col, dr, dc);
        count += count_dir(board, row, col, -dr, -dc);

        if (count >= 4) return 1;
    }
    return 0;
}

// check if board is full
int board_full(char board[ROWS][COLS]) {
    for (int c = 0; c < COLS; c++) {
        if (board[0][c] == '.') return 0;
    }
    return 1;
}
