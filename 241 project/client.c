// client.c - Connect 4 client
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

#define BUF_SIZE 2048

static int recv_line(int sock, char *buf, size_t maxlen) {
    size_t i = 0;
    while (i < maxlen - 1) {
        char ch;
        ssize_t n = recv(sock, &ch, 1, 0);
        if (n <= 0) {
            return -1; // disconnected or error
        }
        if (ch == '\n') break;
        buf[i++] = ch;
    }
    buf[i] = '\0';
    return 0;
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        printf("Usage: %s <server_ip> <port>\n", argv[0]);
        return 1;
    }

    const char *server_ip = argv[1];
    int port = atoi(argv[2]);

    int sock;
    struct sockaddr_in serv_addr;

    if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("socket");
        return 1;
    }

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);

    if (inet_pton(AF_INET, server_ip, &serv_addr.sin_addr) <= 0) {
        perror("inet_pton");
        close(sock);
        return 1;
    }

    if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("connect");
        close(sock);
        return 1;
    }

    printf("Connected to server %s:%d\n", server_ip, port);

    char line[BUF_SIZE];

    while (1) {
        // Read lines until we see something interesting
        if (recv_line(sock, line, sizeof(line)) < 0) {
            printf("Disconnected from server.\n");
            break;
        }

        if (strncmp(line, "WELCOME", 7) == 0) {
            printf("%s\n", line);
        } else if (strcmp(line, "BOARD:") == 0 || strcmp(line, "FINAL BOARD:") == 0) {
            // Next few lines are the board + index line
            printf("\n%s\n", line);
            for (int i = 0; i < 7; i++) { // 6 rows + index line
                if (recv_line(sock, line, sizeof(line)) < 0) {
                    printf("Disconnected while reading board.\n");
                    goto done;
                }
                printf("%s\n", line);
            }
        } else if (strncmp(line, "YOUR_TURN", 9) == 0) {
            printf("%s\n", line);
            printf(">> ");
            fflush(stdout);

            char input[64];
            if (!fgets(input, sizeof(input), stdin)) {
                break;
            }
            // Ensure newline at end for the server
            send(sock, input, strlen(input), 0);
        } else if (strncmp(line, "INVALID_COLUMN", 14) == 0) {
            printf("%s\n", line);
        } else if (strncmp(line, "GAME_OVER", 9) == 0) {
            printf("%s\n", line);
            break;
        } else {
            // Any other line, just show it
            printf("%s\n", line);
        }
    }

done:
    close(sock);
    return 0;
}
