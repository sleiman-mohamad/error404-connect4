#ifndef BOT_HARD_H
#define BOT_HARD_H

#include "engine.h"

// Optional: limit search depth (ply) for speed; 0 = full perfect search.
void setHardBotDepthLimit(int ply);

int getBotMoveHard(char board[ROWS][COLS], char bot, char opponent);

#endif
