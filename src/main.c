#include <stdio.h>
#include "board.h"
int main(void) {
    int b[ROWS][COLS]; init_board(b);
    char buf[512]; board_to_string(b, buf, sizeof(buf));
    printf("%s", buf);
    return 0; // temp CLI will replace later
}
