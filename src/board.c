#include "board.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

static void vappend(char *out, size_t out_size, size_t *written, const char *fmt, ...) {
    if (*written >= out_size) return;
    va_list args; va_start(args, fmt);
    int n = vsnprintf(out + *written, (out_size > *written) ? out_size - *written : 0, fmt, args);
    va_end(args);
    if (n > 0) *written += (size_t)n;
}

void init_board(int board[ROWS][COLS]) {
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++)
            board[r][c] = EMPTY;
}

void board_to_string(int board[ROWS][COLS], char *out, size_t out_size) {
    if (!out || out_size == 0) return;
    size_t written = 0;

    for (int r = 0; r < ROWS; r++) {
        vappend(out, out_size, &written, "| ");
        for (int c = 0; c < COLS; c++) {
            char ch = '.';
            if (board[r][c] == A) ch = 'A';
            else if (board[r][c] == B) ch = 'B';
            vappend(out, out_size, &written, "%c", ch);
            vappend(out, out_size, &written, "%s", (c == COLS - 1) ? " " : " ");
        }
        vappend(out, out_size, &written, "|\n");
    }
    vappend(out, out_size, &written, "  ");
    for (int c = 1; c <= COLS; c++)
        vappend(out, out_size, &written, "%d%s", c, (c == COLS) ? "" : " ");
    vappend(out, out_size, &written, "\n");
}
