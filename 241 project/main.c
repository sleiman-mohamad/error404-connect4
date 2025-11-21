#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <ctype.h>
#include "io.h"
#include "engine.h"
#include "bot_medium.h"
#include "bot_hard.h"

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
    temp[sizeof(temp) - 1] = '\0';
    for (int i = 0; temp[i]; i++) temp[i] = tolower(temp[i]);
    return strcmp(temp, "exit") == 0;
}

int main(void) {
    setbuf(stdout, NULL);
    srand((unsigned)time(NULL));
    printf("Welcome to Connect Four!\n");
    printf("Type 'exit' anytime to quit.\n\n");

    char board[ROWS][COLS];
    char players[2] = {'A', 'B'};
    int current = 0;
    int bot_index = 1;   // 0 or 1; will be chosen by user in bot mode
    char again;

    int bot_enabled = 0;
    char mode[20];
    char difficulty[20] = "easy";  // default

    // Choose mode (bot or multiplayer)
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

            // Choose difficulty
            while (1) {
                printf("Choose difficulty ('easy', 'medium', or 'hard'): ");
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
                } else if (strcmp(difficulty, "medium") == 0) {
                    printf("Starting bot mode (medium difficulty)...\n");
                    break;
                } else if (strcmp(difficulty, "hard") == 0) {
                    printf("Starting bot mode (HARD - very strong)...\n");
                    break;
                } else {
                    printf("Invalid difficulty! Please choose 'easy', 'medium', or 'hard'.\n");
                }
            }
            while (1) {
                printf("Who starts? Type 'bot' or 'player': ");
                char starter[20];
                scanf("%19s", starter);
                for (int i = 0; starter[i]; i++) starter[i] = tolower(starter[i]);
                if (strcmp(starter, "bot") == 0) { bot_index = 0; break; }
                if (strcmp(starter, "player") == 0) { bot_index = 1; break; }
                if (is_exit_command(starter)) { printf("Exiting game. Goodbye!\n"); return 0; }
                printf("Invalid choice. Please type 'bot' or 'player'.\n");
            }
            printf("Starting bot mode. %s begins.\n", bot_index == 0 ? "Bot" : "Human");
            break;
        } else if (strcmp(mode, "multiplayer") == 0) {
            printf("Starting multiplayer mode...\n");
            break;
        } else {
            printf("Invalid mode! Please type 'bot' or 'multiplayer'.\n");
        }
    }

    while (getchar() != '\n'); // clear buffer

    // Main game loop
    do {
        init_board(board);
        int game_over = 0;
        current = bot_enabled ? bot_index : 0; // random start if bot mode

        while (!game_over) {
            print_board(board);

            int col;
            if (bot_enabled && current == bot_index) {
                if (strcmp(difficulty, "hard") == 0)
                    col = getBotMoveHard(board, players[bot_index], players[1 - bot_index]);
                else if (strcmp(difficulty, "medium") == 0)
                    col = getBotMoveMedium(board, players[bot_index], players[1 - bot_index]);
                else
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
                if (bot_enabled && current == bot_index)
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
