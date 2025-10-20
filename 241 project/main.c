#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <ctype.h>
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
    srand(time(NULL)); // seed random once

    char board[ROWS][COLS];
    char players[2] = {'A', 'B'};
    int current = 0;
    char again;

    int bot_enabled = 0;
    char mode[10];
    char difficulty[10];

    printf("Type 'bot' to play against a bot, or '2p' for two players: ");
    scanf("%9s", mode);

    // Convert to lowercase for flexibility
    for (int i = 0; mode[i]; i++)
        mode[i] = tolower(mode[i]);

    if (strcmp(mode, "bot") == 0) {
        bot_enabled = 1;
        printf("Choose difficulty: ");
        scanf("%9s", difficulty);

        for (int i = 0; difficulty[i]; i++)
            difficulty[i] = tolower(difficulty[i]);

        if (strcmp(difficulty, "easy") != 0) {
            printf("Invalid difficulty! Only 'easy' is available for now.\n");
            return 0;  // stop the program
        }

        printf("Starting bot mode (easy difficulty)...\n");
    } 
    else if (strcmp(mode, "2p") == 0) {
        printf("Starting 2-player mode...\n");
    } 
    else {
        printf("Invalid input! Please restart the game and type 'bot' or '2p'.\n");
        return 0;  // stop the program if mode is wrong
    }

    while (getchar() != '\n'); // clear buffer

    
    do {
        init_board(board);
        int game_over = 0;

        while (!game_over) {
            print_board(board);

            int col;
            if (bot_enabled && current == 1) {
                col = getBotMoveEasy(board); // bot’s move
                printf("Bot chooses column %d\n", col);
            } else {
                col = getColumnIn(players[current]);
            }

            int row = place_piece(board, col, players[current]);
            if (row == -1) {
                printf("Column %d is full. Try again.\n", col);
                continue;
            }

            if (check_winner(board, row, col - 1)) {
                print_board(board);
                if (bot_enabled && current == 1)
                    printf("Bot wins!\n");
                else
                    printf("Player %c wins!\n", players[current]);
                game_over = 1;
            } else if (board_full(board)) {
                print_board(board);
                printf("It's a draw!\n");
                game_over = 1;
            } else {
                current = 1 - current;
            }
        }

        printf("Play again? (y/n): ");
        scanf(" %c", &again);
        while (getchar() != '\n');

    } while (again == 'y' || again == 'Y');

    printf("Thanks for playing!\n");
    return 0;
}
