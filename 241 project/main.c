#include <stdio.h>
#include <stdlib.h>
#include "io.h"
#include "engine.h"

void print_board(char board[ROWS][COLS]) {
    
   
    for (int r = 0; r < ROWS; r++) {
        printf(" |");
        for (int c = 0; c < COLS; c++) {
            printf("%c|", board[r][c]);
        }
        printf("\n");
    }
     printf("  1 2 3 4 5 6 7\n");
    
}

int main(void) {
    printf("Welcome to Connect Four!\n");
    char board[ROWS][COLS];
    char players[2] = {'A', 'B'};
    int current = 0;   // 0 = Player A, 1 = Player B
    char again;

    do {
        init_board(board);   // reset the board before each game
        int game_over = 0;

        while (!game_over) {
            print_board(board);

            // get valid column from io.c
            int col = getColumnIn(players[current]);

            // try to place piece (engine.c handles dropping logic)
            int row = place_piece(board, col, players[current]);
            if (row == -1) {
                printf("Column %d is full. Try again.\n", col);
                continue; // ask again without switching player
            }

            // check if that move won
            if (check_winner(board, row, col-1)) {
                print_board(board);
                printf("Player %c wins!\n", players[current]);
                game_over = 1;
            } else if (board_full(board)) {
                print_board(board);
                printf("It's a draw!\n");
                game_over = 1;
            } else {
                //switching players
                current = 1 - current;
            }
        }

        printf("Play again? (y/n): ");
        scanf(" %c", &again);

        // hon bya3mel clear lal characters for the next loop in case.
        while (getchar() != '\n');

    } while (again == 'y' || again == 'Y');

    printf("Thanks for playing!\n");
    return 0;
}
