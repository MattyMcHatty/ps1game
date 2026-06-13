#ifndef TITLE_H
#define TITLE_H

#include "render.h"

typedef enum {
    STATE_TITLE,
    STATE_GAME,
    STATE_MENU,
    STATE_LOADING,
    STATE_LEVEL2,
} GameState;

extern GameState game_state;

void update_title(void);
void draw_title(RenderContext *ctx);
void draw_loading_screen(RenderContext *ctx);

#endif
