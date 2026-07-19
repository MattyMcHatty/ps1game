#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <psxgpu.h>
#include <psxpad.h>
#include "save_menu.h"
#include "render.h"
#include "btn_glyph.h"
#include "camera.h"
#include "player.h"
#include "title.h"
#include "memcard.h"
#include "savegame.h"
#include "save_point.h"
#include "reception.h"

extern volatile uint8_t pad_buff[2][34];
extern volatile size_t  pad_buff_len[2];

/* ---- Flow screens ---------------------------------------------------------- */
enum {
    SM_CARD_SELECT,   /* choose memory card 1 / 2                */
    SM_FILE_SELECT,   /* choose "new save" or a save to overwrite */
    SM_CONFIRM,       /* confirm an overwrite                     */
    SM_SAVING,        /* writing to the card (blocking)           */
    SM_RESULT,        /* "save complete" / error                  */
};

/* ---- Panel layout (screen pixels, 320x240) --------------------------------- */
#define PANEL_X    10
#define PANEL_Y    45
#define PANEL_W   300
#define PANEL_H   150
#define OPT_Y0   (PANEL_Y + 34)
#define OPT_DY    16
#define MAX_OPTS  16

/* OT layers within the menu-reserved range (< SCENE_OT_MIN); lower = on top. */
#define OT_DIM     15   /* full-screen dim                */
#define OT_PANEL   12   /* panel fill                     */
#define OT_FRAME   10   /* panel + option outlines        */
#define OT_HILITE   8   /* cursor row highlight           */
#define OT_TEXT     4   /* FntSort text (front-most)      */

static int  sm_screen;
static int  sm_port;          /* 0 = card slot 1, 1 = slot 2 */
static int  sm_cursor;
static int  sm_dpad_prev;

static SaveSlotInfo sm_slots[SAVE_MAX_SLOTS];
static int  sm_slot_count;
static int  sm_free_block;

/* Built option list for SM_FILE_SELECT. */
static char sm_label[MAX_OPTS][40];
static int  sm_block[MAX_OPTS];   /* target card block for this option */
static int  sm_isnew[MAX_OPTS];
static int  sm_opt_count;

static int  sm_target_block;
static int  sm_target_isnew;
static int  sm_save_delay;        /* defers the write one frame so "SAVING" shows */
static int  sm_result;            /* MC_* code from the last write               */

static const char *area_name(int area) {
    switch (area) {
        case STATE_RECEPTION:      return "RECEPTION";
        case STATE_KITCHEN_DINING: return "KITCHEN";
        case STATE_DELIVERY_AREA:  return "DELIVERY";
        default:                   return "MANSION";
    }
}

/* ---- Small draw helpers (same primitives as the pause menu) ---------------- */
static void draw_rect(RenderContext *ctx, int x, int y, int w, int h,
                      uint8_t r, uint8_t g, uint8_t b, int ot_idx) {
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    if (ctx->next_packet + sizeof(TILE) > buf_end) return;
    TILE *t = (TILE *)ctx->next_packet;
    setTile(t);
    setXY0(t, x, y);
    setWH(t, w, h);
    setRGB0(t, r, g, b);
    addPrim(&ctx->buffers[ctx->active_buffer].ot[ot_idx], t);
    ctx->next_packet += sizeof(TILE);
}

static void draw_outline(RenderContext *ctx, int x, int y, int w, int h,
                         uint8_t r, uint8_t g, uint8_t b, int ot_idx) {
    draw_rect(ctx, x,       y,       w, 1, r, g, b, ot_idx);
    draw_rect(ctx, x,       y+h-1,   w, 1, r, g, b, ot_idx);
    draw_rect(ctx, x,       y,       1, h, r, g, b, ot_idx);
    draw_rect(ctx, x+w-1,   y,       1, h, r, g, b, ot_idx);
}

static void draw_text(RenderContext *ctx, int x, int y, const char *s) {
    uint32_t *ot = ctx->buffers[ctx->active_buffer].ot;
    ctx->next_packet = FntSort(&ot[OT_TEXT], ctx->next_packet, x, y, s);
}

/* ---- Flow transitions ------------------------------------------------------ */

/* Seed the d-pad/button edge detector from the current pad so the press that
   arrives with a new screen isn't re-consumed. */
static void seed_buttons(void) {
    if (pad_buff_len[0]) {
        PadResponse *pad = (PadResponse *)pad_buff[0];
        sm_dpad_prev = ~pad->btn;
    } else {
        sm_dpad_prev = 0;
    }
}

static void enter_file_select(void) {
    sm_slot_count = savegame_list(sm_port, sm_slots, SAVE_MAX_SLOTS);
    if (sm_slot_count < 0) {                 /* card vanished mid-read */
        sm_result = MC_NO_CARD;
        sm_screen = SM_RESULT;
        seed_buttons();
        return;
    }
    sm_free_block = savegame_free_block(sm_port);
    if (sm_free_block < 0) sm_free_block = 0;

    /* Build the option list: an optional "new save", then each existing save. */
    sm_opt_count = 0;
    if (sm_free_block > 0) {
        strcpy(sm_label[sm_opt_count], "* NEW SAVE *");
        sm_block[sm_opt_count] = sm_free_block;
        sm_isnew[sm_opt_count] = 1;
        sm_opt_count++;
    }
    int i;
    for (i = 0; i < sm_slot_count && sm_opt_count < MAX_OPTS; i++) {
        snprintf(sm_label[sm_opt_count], sizeof(sm_label[0]), "%s", sm_slots[i].title);
        sm_block[sm_opt_count] = sm_slots[i].block;
        sm_isnew[sm_opt_count] = 0;
        sm_opt_count++;
    }

    sm_cursor = 0;
    sm_screen = SM_FILE_SELECT;
    seed_buttons();
}

static void close_menu(void) {
    /* Re-arm the interaction edges so the confirming Circle/Cross doesn't leak
       into the room and immediately re-open something. */
    save_point_arm();
    reception_door_arm();
    game_state = current_area;
}

/* Perform the actual card write (called once "SAVING" is already on screen). */
static void do_save(void) {
    SaveData sd;
    char     title[33];
    /* This write is save number player_save_count+1 of the playthrough. The
       counter is global — overwrites and saves to other slots/cards all keep
       counting up — and lives in the save data too, so a future load restores
       it. It is only committed once the card write actually succeeds. */
    int count = player_save_count + 1;
    savegame_capture(&sd);
    sd.counter = (uint32_t)count;
    /* On-card title, e.g. "3. RECEPTION - 005 SAVES". */
    snprintf(title, sizeof(title), "%d. %s - %03d SAVES",
             sm_target_block, area_name(sd.area), count);
    sm_result = savegame_write(sm_port, sm_target_block, &sd, title);
    if (sm_result == MC_OK)
        player_save_count = count;
    sm_screen = SM_RESULT;
    seed_buttons();
}

/* ---- Public API ------------------------------------------------------------ */

void save_menu_open(void) {
    sm_screen = SM_CARD_SELECT;
    sm_cursor = 0;
    sm_port   = 0;
    seed_buttons();
}

void save_menu_update(void) {
    /* The blocking write happens here, one frame after we switch to SM_SAVING,
       so the "SAVING" screen has already been flipped to the display. */
    if (sm_screen == SM_SAVING) {
        if (sm_save_delay > 0) { sm_save_delay--; return; }
        do_save();
        return;
    }

    if (!pad_buff_len[0]) return;
    PadResponse *pad = (PadResponse *)pad_buff[0];
    uint16_t btn     = ~pad->btn;
    uint16_t pressed = btn & ~sm_dpad_prev;
    sm_dpad_prev     = btn;

    int confirm = (pressed & PAD_CIRCLE) ? 1 : 0;
    int back    = (pressed & PAD_CROSS)  ? 1 : 0;
    int cancel  = (pressed & PAD_START)  ? 1 : 0;

    switch (sm_screen) {
    case SM_CARD_SELECT:
        if (pressed & PAD_UP)   sm_cursor = (sm_cursor + 1) & 1;
        if (pressed & PAD_DOWN) sm_cursor = (sm_cursor + 1) & 1;
        if (cancel || back) { close_menu(); return; }
        if (confirm) {
            sm_port = sm_cursor;
            memcard_begin();
            int present = memcard_present(sm_port);
            memcard_end();
            if (!present) { sm_result = MC_NO_CARD; sm_screen = SM_RESULT; seed_buttons(); return; }
            enter_file_select();
        }
        break;

    case SM_FILE_SELECT:
        if (sm_opt_count > 0) {
            if (pressed & PAD_UP)   sm_cursor = (sm_cursor + sm_opt_count - 1) % sm_opt_count;
            if (pressed & PAD_DOWN) sm_cursor = (sm_cursor + 1) % sm_opt_count;
        }
        if (back)   { sm_screen = SM_CARD_SELECT; sm_cursor = sm_port; seed_buttons(); return; }
        if (cancel) { close_menu(); return; }
        if (confirm && sm_opt_count > 0) {
            sm_target_block = sm_block[sm_cursor];
            sm_target_isnew = sm_isnew[sm_cursor];
            if (sm_target_isnew) {
                sm_save_delay = 1;
                sm_screen     = SM_SAVING;
            } else {
                sm_screen = SM_CONFIRM;
                seed_buttons();
            }
        }
        break;

    case SM_CONFIRM:
        if (confirm) { sm_save_delay = 1; sm_screen = SM_SAVING; }
        else if (back || cancel) { sm_screen = SM_FILE_SELECT; seed_buttons(); }
        break;

    case SM_RESULT:
        if (confirm || back || cancel) { close_menu(); return; }
        break;

    default:
        break;
    }
}

void save_menu_draw(RenderContext *ctx) {
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;

    /* Dimmed, semi-transparent backdrop over the frozen room (as the pause menu). */
    if (ctx->next_packet + sizeof(TILE) <= buf_end) {
        TILE *t = (TILE *)ctx->next_packet;
        setTile(t);
        setSemiTrans(t, 1);
        setXY0(t, 0, 0);
        setWH(t, 320, 240);
        setRGB0(t, 18, 14, 24);
        addPrim(&ctx->buffers[ctx->active_buffer].ot[OT_DIM], t);
        ctx->next_packet += sizeof(TILE);
    }
    if (ctx->next_packet + sizeof(DR_TPAGE) <= buf_end) {
        DR_TPAGE *dp = (DR_TPAGE *)ctx->next_packet;
        setDrawTPage(dp, 0, 0, getTPage(0, 0, 0, 0));   /* abr=0 = 50% blend */
        addPrim(&ctx->buffers[ctx->active_buffer].ot[OT_DIM], dp);
        ctx->next_packet += sizeof(DR_TPAGE);
    }

    /* Panel */
    draw_rect(ctx, PANEL_X, PANEL_Y, PANEL_W, PANEL_H, 24, 20, 34, OT_PANEL);
    draw_outline(ctx, PANEL_X, PANEL_Y, PANEL_W, PANEL_H, 120, 90, 170, OT_FRAME);
    draw_rect(ctx, PANEL_X + 8, PANEL_Y + 22, PANEL_W - 16, 1, 90, 70, 120, OT_FRAME);

    switch (sm_screen) {
    case SM_CARD_SELECT: {
        draw_text(ctx, PANEL_X + 10, PANEL_Y + 8, "SAVE - SELECT CARD");
        static const char *cards[2] = { "MEMORY CARD 1", "MEMORY CARD 2" };
        int i;
        for (i = 0; i < 2; i++) {
            int y = OPT_Y0 + i * OPT_DY;
            if (i == sm_cursor)
                draw_rect(ctx, PANEL_X + 8, y - 2, PANEL_W - 16, 12, 70, 55, 110, OT_HILITE);
            draw_text(ctx, PANEL_X + 14, y, cards[i]);
        }
        btn_prompt_draw(ctx, PANEL_X + 10, PANEL_Y + PANEL_H - 14,
                        BTN_CIRCLE " SELECT   " BTN_CROSS " BACK", OT_TEXT);
        break;
    }
    case SM_FILE_SELECT: {
        draw_text(ctx, PANEL_X + 10, PANEL_Y + 8, "SAVE - SELECT FILE");
        int i;
        for (i = 0; i < sm_opt_count; i++) {
            int y = OPT_Y0 + i * OPT_DY;
            if (i == sm_cursor)
                draw_rect(ctx, PANEL_X + 8, y - 2, PANEL_W - 16, 12, 70, 55, 110, OT_HILITE);
            draw_text(ctx, PANEL_X + 14, y, sm_label[i]);
        }
        if (sm_opt_count == 0)
            draw_text(ctx, PANEL_X + 14, OPT_Y0, "CARD FULL");
        btn_prompt_draw(ctx, PANEL_X + 10, PANEL_Y + PANEL_H - 14,
                        BTN_CIRCLE " SAVE   " BTN_CROSS " BACK", OT_TEXT);
        break;
    }
    case SM_CONFIRM: {
        draw_text(ctx, PANEL_X + 10, PANEL_Y + 8, "OVERWRITE?");
        draw_text(ctx, PANEL_X + 14, OPT_Y0,          sm_label[sm_cursor]);
        draw_text(ctx, PANEL_X + 14, OPT_Y0 + OPT_DY, "This will be replaced.");
        btn_prompt_draw(ctx, PANEL_X + 10, PANEL_Y + PANEL_H - 14,
                        BTN_CIRCLE " YES   " BTN_CROSS " NO", OT_TEXT);
        break;
    }
    case SM_SAVING: {
        draw_text(ctx, PANEL_X + 10, PANEL_Y + 8, "SAVING...");
        draw_text(ctx, PANEL_X + 14, OPT_Y0,          "Writing to memory card.");
        draw_text(ctx, PANEL_X + 14, OPT_Y0 + OPT_DY, "Do not remove the card.");
        break;
    }
    case SM_RESULT: {
        const char *msg1, *msg2;
        if (sm_result == MC_OK)           { msg1 = "SAVE COMPLETE";  msg2 = "Your progress is saved."; }
        else if (sm_result == MC_NO_CARD) { msg1 = "NO MEMORY CARD"; msg2 = "None found in this slot."; }
        else                              { msg1 = "SAVE FAILED";    msg2 = "The card may be full/bad."; }
        draw_text(ctx, PANEL_X + 10, PANEL_Y + 8, msg1);
        draw_text(ctx, PANEL_X + 14, OPT_Y0, msg2);
        btn_prompt_draw(ctx, PANEL_X + 10, PANEL_Y + PANEL_H - 14,
                        BTN_CIRCLE " OK", OT_TEXT);
        break;
    }
    default: break;
    }
}
