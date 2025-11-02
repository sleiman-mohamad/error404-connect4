#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "bot_medium.h"



int getBotMoveMedium(char board[ROWS][COLS], char bot, char opponent) {
    srand(time(NULL));

    // Helper array to prioritize center columns
    int column_order[] = {4, 3, 5, 2, 6, 1, 7};

    // ⿡ Try to play a winning move
    for (int c = 1; c <= COLS; c++) {
        int r = place_piece(board, c, bot);
        if (r != -1) {
            if (check_winner(board, r, c - 1)) {
                board[r][c - 1] = '.';
                return c; // winning column found
            }
            board[r][c - 1] = '.';
        }
    }

    // ⿢ Try to block opponent's winning move
    for (int c = 1; c <= COLS; c++) {
        int r = place_piece(board, c, opponent);
        if (r != -1) {
            if (check_winner(board, r, c - 1)) {
                board[r][c - 1] = '.';
                return c; // block opponent
            }
            board[r][c - 1] = '.';
        }
    }

    // ⿣ Prefer center and safe moves
    for (int i = 0; i < 7; i++) {
        int c = column_order[i];
        if (board[0][c - 1] != '.')
            continue; // skip full columns

        int r = place_piece(board, c, bot);
        if (r == -1)
            continue;

        // Check if this move gives the opponent a win next turn
        int danger = 0;
        for (int oc = 1; oc <= COLS; oc++) {
            int orow = place_piece(board, oc, opponent);
            if (orow != -1) {
                if (check_winner(board, orow, oc - 1)) {
                    danger = 1; // unsafe move
                    board[orow][oc - 1] = '.';
                    break;
                }
                board[orow][oc - 1] = '.';
            }
        }

        board[r][c - 1] = '.'; // undo simulated move

        if (!danger)
            return c; // safe and strategic column
    }

    // ⿤ If all moves risky, pick first available center-most column
    for (int i = 0; i < 7; i++) {
        int c = column_order[i];
        if (board[0][c - 1] == '.')
            return c;
    }

    // fallback
   return 1;
}