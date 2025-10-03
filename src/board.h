#ifndef BOARD_H
#define BOARD_H

#include <stddef.h>

#define ROWS 6
#define COLS 7

typedef enum { EMPTY = 0, A = 1, B = 2 } Player;

void init_board(int board[ROWS][COLS]);
void board_to_string(int board[ROWS][COLS], char *out, size_t out_size);

#endif