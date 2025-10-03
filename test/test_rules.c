#include <assert.h>
#include <stdio.h>
#include "../src/rules.h"

static void test_initial_state() {
    int b[ROWS][COLS]; init_board(b);
    assert(check_win(b, A) == 0);
    assert(check_win(b, B) == 0);
    assert(is_draw(b) == 0);
}

static void test_vertical_win() {
    int b[ROWS][COLS]; init_board(b);
    for (int i = 0; i < 4; i++) assert(place_checker(b, 3, A) == (ROWS-1 - i));
    assert(check_win(b, A) == 1);
}

static void test_horizontal_win() {
    int b[ROWS][COLS]; init_board(b);
    // Bottom row: cols 1..4 (0..3)
    assert(place_checker(b, 0, B) == ROWS-1);
    assert(place_checker(b, 1, B) == ROWS-1);
    assert(place_checker(b, 2, B) == ROWS-1);
    assert(place_checker(b, 3, B) == ROWS-1);
    assert(check_win(b, B) == 1);
}

static void test_diag_dr_win() { // ↘
    int b[ROWS][COLS]; init_board(b);
    // Build a staircase for A on cols 0..3
    place_checker(b,0,A);                 // r5,c0
    place_checker(b,1,B); place_checker(b,1,A); // r4,c1
    place_checker(b,2,B); place_checker(b,2,B); place_checker(b,2,A); // r3,c2
    place_checker(b,3,B); place_checker(b,3,B); place_checker(b,3,B); place_checker(b,3,A); // r2,c3
    assert(check_win(b, A) == 1);
}

static void test_full_column_and_invalid_col() {
    int b[ROWS][COLS]; init_board(b);
    assert(place_checker(b, -1, A) == -1);
    assert(place_checker(b, COLS, A) == -1);
    for (int i = 0; i < ROWS; i++) assert(place_checker(b, 0, A) == ROWS-1-i);
    assert(place_checker(b, 0, A) == -1); // now full
}

int main(void) {
    test_initial_state();
    test_vertical_win();
    test_horizontal_win();
    test_diag_dr_win();
    test_full_column_and_invalid_col();
    printf("All engine tests passed.\n");
    return 0;
}