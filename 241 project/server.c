// server.c – Connect 4 online multiplayer server

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#include "engine.h"   // your board logic
#include "io.h"       // for ROWS, COLS, etc.

#define PORT 8080
#define BUF 2048

// send helper
void send_line(int sock, const char *msg) {
    send(sock, msg, strlen(msg), 0);
    send(sock, "\n", 1, 0);
}

// draw the board as text
void send_board(int sock, char board[ROWS][COLS], const char *type) {
    char line[BUF];
    send_line(sock, type);  

    for (int r = 0; r < ROWS; r++) {
        sprintf(line, " |%c|%c|%c|%c|%c|%c|%c|",
                board[r][0], board[r][1], board[r][2],
                board[r][3], board[r][4], board[r][5], board[r][6]);
        send_line(sock, line);
    }
    send_line(sock, "  1 2 3 4 5 6 7");
}

// receive 1 line
int recv_line(int sock, char *buf, int max) {
    int i = 0;
    while (i < max - 1) {
        char ch;
        int n = recv(sock, &ch, 1, 0);
        if (n <= 0) return -1;
        if (ch == '\n') break;
        buf[i++] = ch;
    }
    buf[i] = '\0';
    return 1;
}

int main() {
    int serv, player[2];
    struct sockaddr_in addr;

    serv = socket(AF_INET, SOCK_STREAM, 0);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(PORT);

    bind(serv, (struct sockaddr *)&addr, sizeof(addr));
    listen(serv, 2);

    printf("SERVER: Waiting for 2 players on port %d...\n", PORT);

    player[0] = accept(serv, NULL, NULL);
    send_line(player[0], "WELCOME Player 1 (A)");

    player[1] = accept(serv, NULL, NULL);
    send_line(player[1], "WELCOME Player 2 (B)");

    printf("SERVER: Both players connected.\n");

    char board[ROWS][COLS];
    init_board(board);

    int turn = 0; // 0 = A, 1 = B
    char players[2] = {'A','B'};
    char buf[BUF];

    while (1) {
        // send board update
        send_board(player[0], board, "BOARD:");
        send_board(player[1], board, "BOARD:");

        // tell whose turn it is
        send_line(player[turn], "YOUR_TURN");

        // receive move
        if (recv_line(player[turn], buf, BUF) <= 0) break;

        int col = atoi(buf);
        int row = place_piece(board, col, players[turn]);

        if (row == -1) {
            send_line(player[turn], "INVALID_COLUMN");
            continue;
        }

        // check win
        if (check_winner(board, row, col - 1)) {
            send_board(player[0], board, "FINAL BOARD:");
            send_board(player[1], board, "FINAL BOARD:");

            if (turn == 0) {
                send_line(player[0], "GAME_OVER: Player A wins!");
                send_line(player[1], "GAME_OVER: Player A wins!");
            } else {
                send_line(player[0], "GAME_OVER: Player B wins!");
                send_line(player[1], "GAME_OVER: Player B wins!");
            }
            break;
        }

        // check draw
        if (board_full(board)) {
            send_board(player[0], board, "FINAL BOARD:");
            send_board(player[1], board, "FINAL BOARD:");
            send_line(player[0], "GAME_OVER: Draw!");
            send_line(player[1], "GAME_OVER: Draw!");
            break;
        }

        turn ^= 1; // switch turns
    }

    close(player[0]);
    close(player[1]);
    close(serv);
    return 0;
}
