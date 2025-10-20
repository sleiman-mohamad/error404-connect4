#ifndef ENGINE_H
#define ENGINE_H

#define ROWS 6
#define COLS 7

void init_board(char board[ROWS][COLS]);
int place_piece(char board[ROWS][COLS], int col, char player);
int getBotMoveEasy(char board[ROWS][COLS]);

int check_winner(char board[ROWS][COLS], int row, int col);
int board_full(char board[ROWS][COLS]);

#endif
