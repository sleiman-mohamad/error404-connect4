#ifndef RULES_H
#define RULES_H

#include "board.h"

int place_checker(int board[ROWS][COLS], int col, Player p); // returns row or -1
int check_win(int board[ROWS][COLS], Player p);              // 1 if win else 0
int is_draw(int board[ROWS][COLS]);                          // 1 if full else 0

#endif