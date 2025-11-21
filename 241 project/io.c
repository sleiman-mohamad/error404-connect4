#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include "io.h"

#define COLS 7


static void clear_input_buffer(void) {
    int ch;
    while ((ch = getchar()) != '\n' && ch != EOF);
}

int getColumnIn(char player) {
     int col;
    int result;

    while (1) {
        printf("Player %c, choose column (1-7): ", player);

        result = scanf("%d", &col);

        
        if (result != 1) {
            printf("Invalid input! Please enter a number from 1 to 7.\n");
            clear_input_buffer();
            continue;
        }

        
        if (col < 1 || col > 7) {
            printf("Column out of range. Try again.\n");
            continue;
        }

        return col;  
    }
}
