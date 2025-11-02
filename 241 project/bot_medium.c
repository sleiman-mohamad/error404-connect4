#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "bot_medium.h"

static int count_dir(char board[ROWS][COLS], int r, int c, int dr, int dc, char player) {
    int count = 0;
    int rr = r + dr, cc = c + dc;
    while (rr >= 0 && rr < ROWS && cc >= 0 && cc < COLS && board[rr][cc] == player) {
        count++;
        rr += dr; cc += dc;
    }
    return count;
}

static int creates_threat(char board[ROWS][COLS], int row, int col, char player) {
    int dirs[4][2] = {{0,1},{1,0},{1,1},{1,-1}};
    for (int i = 0; i < 4; i++) {
        int count = 1;
        count += count_dir(board,row,col,dirs[i][0],dirs[i][1],player);
        count += count_dir(board,row,col,-dirs[i][0],-dirs[i][1],player);
        if (count == 3) return 1;
    }
    return 0;
}

static int opponent_can_win_next(char board[ROWS][COLS], char opponent) {
    for (int c = 1; c <= COLS; c++) {
        int r = place_piece(board, c, opponent);
        if (r != -1) {
            int win = check_winner(board, r, c - 1);
            board[r][c - 1] = '.';
            if (win) return 1;
        }
    }
    return 0;
}

int getBotMoveMedium(char board[ROWS][COLS], char bot, char opponent) {
    srand(time(NULL));
    int order[] = {4, 3, 5, 2, 6, 1, 7};

    for (int c = 1; c <= COLS; c++) {
        int r = place_piece(board, c, bot);
        if (r != -1) {
            if (check_winner(board, r, c - 1)) {
                board[r][c - 1] = '.';
                return c;
            }
            board[r][c - 1] = '.';
        }
    }

    for (int c = 1; c <= COLS; c++) {
        int r = place_piece(board, c, opponent);
        if (r != -1) {
            if (check_winner(board, r, c - 1)) {
                board[r][c - 1] = '.';
                return c;
            }
            board[r][c - 1] = '.';
        }
    }

    int best_col = -1;
    for (int i = 0; i < 7; i++) {
        int c = order[i];
        if (board[0][c - 1] != '.') continue;

        int r = place_piece(board, c, bot);
        if (r == -1) continue;

        if (r + 1 < ROWS && board[r + 1][c - 1] == opponent) {
            board[r][c - 1] = '.';
            continue;
        }

        if (opponent_can_win_next(board, opponent)) {
            board[r][c - 1] = '.';
            continue;
        }

        if (creates_threat(board, r, c - 1, bot)) {
            board[r][c - 1] = '.';
            return c;
        }

        if (best_col == -1) best_col = c;

        board[r][c - 1] = '.';
    }

    if (best_col != -1)
        return best_col;

    int valid[COLS], n = 0;
    for (int c = 1; c <= COLS; c++) {
        if (board[0][c - 1] == '.') valid[n++] = c;
    }
    if (n == 0) return 1;
    return valid[rand() % n];
}