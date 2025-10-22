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

// Helper to check if input is "exit"
int is_exit_command(const char *input) {
    char temp[50];
    strncpy(temp, input, sizeof(temp));
    temp[sizeof(temp)-1] = '\0';
    for (int i = 0; temp[i]; i++) temp[i] = tolower(temp[i]);
    return strcmp(temp, "exit") == 0;
}

int main(void) {
    printf("Welcome to Connect Four!\n");
    printf("Type 'exit' anytime to quit.\n\n");

    

    char board[ROWS][COLS];
    char players[2] = {'A', 'B'};
    int current = 0;
    char again;

    int bot_enabled = 0;
    char mode[20];
    char difficulty[20];

    // ✅ Keep asking until valid mode or exit
    while (1) {
        printf("Type 'bot' to play against a bot, or 'multiplayer' for two players: ");
        scanf("%19s", mode);

        if (is_exit_command(mode)) {
            printf("Exiting game. Goodbye!\n");
            return 0;
        }

        for (int i = 0; mode[i]; i++)
            mode[i] = tolower(mode[i]);

        if (strcmp(mode, "bot") == 0) {
            bot_enabled = 1;

            // ✅ Keep asking until valid difficulty or exit
            while (1) {
                printf("Choose difficulty (only 'easy' available): ");
                scanf("%19s", difficulty);

                if (is_exit_command(difficulty)) {
                    printf("Exiting game. Goodbye!\n");
                    return 0;
                }

                for (int i = 0; difficulty[i]; i++)
                    difficulty[i] = tolower(difficulty[i]);

                if (strcmp(difficulty, "easy") == 0) {
                    printf("Starting bot mode (easy difficulty)...\n");
                    break;
                } else {
                    printf("Invalid difficulty! Only 'easy' is available.\n");
                }
            }
            break;
        } 
        else if (strcmp(mode, "multiplayer") == 0) {
            printf("Starting multiplayer mode...\n");
            break;
        } 
        else {
            printf("Invalid input! Please type 'bot' or 'multiplayer'.\n");
        }
    }

    while (getchar() != '\n'); // clear buffer

    do {
        init_board(board);
        int game_over = 0;

        while (!game_over) {
            print_board(board);

            int col;
            if (bot_enabled && current == 1) {
                col = getBotMoveEasy(board);
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

        printf("Play again? (y/n or 'exit'): ");
        scanf(" %c", &again);
        if (tolower(again) == 'e') {
            printf("Exiting game. Goodbye!\n");
            break;
        }

        while (getchar() != '\n'); // clear leftover input

    } while (again == 'y' || again == 'Y');

    printf("Thanks for playing!\n");
    return 0;
}
