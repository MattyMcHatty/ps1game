#ifndef TITLE_H
#define TITLE_H

#include "render.h"

typedef enum {
    STATE_TITLE,
    STATE_GAME,
    STATE_MENU
} GameState;

extern GameState game_state;

void update_title(void);
void draw_title(RenderContext *ctx);

#endif
