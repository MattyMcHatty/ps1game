#include <stdint.h>
#include <stdlib.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <psxetc.h>
#include <psxcd.h>
#include <psxpad.h>
#include "render.h"
#include "title.h"
#include "btn_glyph.h"
#include "memcard.h"
#include "savegame.h"
#include "debug_opts.h"
#include "intro.h"
#include "door.h"
#include "sound.h"

extern volatile uint8_t pad_buff[2][34];
extern volatile size_t  pad_buff_len[2];

/* ---- Debug menu (opened with Select on the title screen) -------------------
   Two columns side by side: rooms to jump to on the left, cheat toggles on the
   right (see debug_opts.h). Left/Right switches column and each keeps its own
   cursor, so moving across never loses your place in the room list. */

enum { DBG_COL_LEVELS = 0, DBG_COL_OPTS };

static int debug_menu_open   = 0;
static int debug_menu_cursor = 0;    /* row in the left (level) column  */
static int debug_opt_cursor  = 0;    /* row in the right (option) column */
static int debug_col         = DBG_COL_LEVELS;
static int debug_scroll      = 0;    /* first VISIBLE row of the level column */
static int level_select_fnt  = -1;
static int debug_fnt         = -1;   /* left column, top-left  */

/* The right column is drawn with btn_prompt_draw (FntSort straight into the OT)
   rather than a font stream of its own. FntOpen is capped at EIGHT streams
   SDK-wide and does not bounds-check: the project already opens exactly eight
   (gameover, notify, items, weapons, menu, level_select, load_list, debug), so a
   ninth silently writes a stream struct past the end of its array and corrupts
   whatever bss follows. Do not add another FntOpen without freeing one first. */
#define DBG_OPT_X       144   /* left edge of the options column   */
#define DBG_OPT_BOX_X   152   /* the [ ] checkbox                  */
#define DBG_OPT_NAME_X  184   /* the label                         */
#define DBG_OPT_TOP_Y    24   /* first row: below "OPTIONS" + blank */
/* The footer is pinned to the bottom of the screen rather than trailing the
   longer of the two lists, so it does not jump around as entries are added.
   The taller column (levels) SCROLLS to stay clear of it — see DBG_LIST_ROWS. */
#define DBG_FOOTER_Y    224

/* Rows of the level column that are on screen at once. The column opens at y=8
   with "LEVEL SELECT" and a blank row, so the list itself runs from y=24, and
   the last row has to end by DBG_FOOTER_Y - 8 to leave a clear line above the
   button prompt. The list is longer than that (the rooms plus the chapter
   headings below), so it scrolls with the cursor instead of running off the
   bottom of the screen underneath the footer, which is what the last few
   entries used to do. */
#define DBG_LIST_TOP_Y   24
#define DBG_LIST_ROWS   ((DBG_FOOTER_Y - 8 - DBG_LIST_TOP_Y) / 8)

/* ---- Start menu (opened with Start): New Game / Load Game ------------------
   Load Game walks card slot -> save file, reads the chosen SaveData, stages it
   (savegame_stage_load) and routes into the saved area exactly like the level
   select does; main.c applies the staged state once the area is initialised. */
enum { TM_CLOSED, TM_MAIN, TM_CARD, TM_FILE };
static int tmenu        = TM_CLOSED;
static int tmenu_cursor = 0;
static int tmenu_port   = 0;                 /* chosen card slot (0/1) */
static const char *tmenu_msg = 0;            /* inline error line, if any */
static SaveSlotInfo tmenu_slots[SAVE_MAX_SLOTS];
static int tmenu_slot_count = 0;
static int load_list_fnt    = -1;            /* wider stream for save titles */

/* The menu sits in the band under the title stack (which ends at y=133) and
   above the corner captions (which start at y=224). The debug font is 8x8 and
   FntPrint steps one line per 8px, so the band holds ten rows.

   Its window is 22 characters wide starting at TMENU_X, giving a centre line of
   TMENU_X + 88 = 168. FntPrint is left-aligned and the window cannot move per
   state, so each line is centred on the SCREEN centre (160) by padding it with
   leading spaces; that only buys 8px steps, hence the odd half-character
   offsets noted against the strings below. */
#define TMENU_X          80
#define TMENU_Y         144
#define TMENU_ROW         8   /* 8x8 font: one printed line */

/* One prompt, one place, for all three menu states — TM_MAIN, TM_CARD and
   TM_FILE each lay their rows out above it rather than trailing it after their
   own last line. It is 15 characters, so x=100 centres it on 160, and it sits
   one blank row above the captions. */
#define TMENU_PROMPT    BTN_CIRCLE ":SELECT " BTN_CROSS ":BACK"
#define TMENU_PROMPT_X  100
#define TMENU_PROMPT_Y  212

/* Save rows that fit between the save list's first entry (heading + blank row
   below TMENU_Y) and the prompt. Six at the current spacing; a full card holds
   fifteen saves, so the list scrolls. */
#define TMENU_FILE_ROWS ((TMENU_PROMPT_Y - (TMENU_Y + 2 * TMENU_ROW)) / TMENU_ROW)

/* The cursor is a dash on either side of the highlighted entry ("- NEW GAME -").
   Both halves have a blank stand-in so an unselected row occupies exactly the
   same cells — the whole block would otherwise shuffle sideways as the cursor
   moves. Each row's leading pad is one character smaller than it was for the
   old single asterisk, which keeps the block sitting where it always did. */
#define TMENU_MARK(sel)  ((sel) ? "-" : " ")

static const char *const level_names[] = {
    "DELIVERY AREA",
    "KITCHEN DINING",
    "RECEPTION",
    "PIANO ROOM",
    "CONSERVATORY",
    "2F HALL",
    "MASTER BEDROOM",
    "EAST HALL",
    "LIBRARY",
    "EAST STAIRWELL",
    "ATC STAIRWELL",
    "ATTIC EXIT",
    "GARDEN STAIRS",
    "GRDN COURTYARD",
    "FOUNTAIN SQR",
    "OUT CATACOMBS",
    "MAZE ONE",
    "MAZE TWO",
    "REAR GATE",
    "WEST CORRIDOR",
    "LIBRARY DSTRYD",
    "STABLES",
    "KEYSTONE MAZE",
    "GREENHOUSE",
    "CHAIN ROOM",
    "THE HATCH",
    /* >>> NO "ASAG ARENA" ROW. <<< The fight is LOCKED OUT of this build, and
       this list was one of its two ways in — the other was The Hatch's drop
       (src/main.c, at hatch_puzzle_drop_done()), which now ends the build on the
       sign-off screen instead. The arena itself is untouched and still builds:
       STATE_ASAG_ARENA still exists in title.h and every branch that handles it
       is still in main.c. Put the row back HERE and in level_states /
       level_pending below — all three, in the same position — to reopen it. */
};
#define LEVEL_SELECT_COUNT ((int)(sizeof(level_names) / sizeof(level_names[0])))

/* Target game state for each entry (index matches level_names). Rooms set up by
   STATE_LOADING use level_pending[] below to say which area to switch to. */
static const GameState level_states[LEVEL_SELECT_COUNT] = {
    STATE_DELIVERY_AREA,
    STATE_LOADING,
    STATE_LOADING,
    STATE_LOADING,
    STATE_LOADING,
    STATE_LOADING,
    STATE_LOADING,
    STATE_LOADING,
    STATE_LOADING,
    STATE_LOADING,
    STATE_LOADING,
    STATE_LOADING,
    STATE_LOADING,
    STATE_LOADING,
    STATE_LOADING,
    STATE_LOADING,
    STATE_LOADING,
    STATE_LOADING,
    STATE_LOADING,
    STATE_LOADING,
    STATE_LOADING,
    STATE_LOADING,
    STATE_LOADING,
    STATE_LOADING,
    STATE_LOADING,
    STATE_LOADING,
};

/* For STATE_LOADING entries, the area STATE_LOADING should switch to. */
static const GameState level_pending[LEVEL_SELECT_COUNT] = {
    STATE_DELIVERY_AREA,    /* unused (not a STATE_LOADING entry) */
    STATE_KITCHEN_DINING,
    STATE_RECEPTION,
    STATE_PIANO_ROOM,
    STATE_CONSERVATORY,
    STATE_2F_HALL,
    STATE_MASTER_BEDROOM,
    STATE_EAST_HALL,
    STATE_LIBRARY,
    STATE_EAST_STAIRWELL,
    STATE_ATTIC_STAIRWELL,
    STATE_ATTIC_EXIT,
    STATE_GARDEN_STAIRS,
    STATE_GARDEN_COURTYARD,
    STATE_FOUNTAIN_SQUARE,
    STATE_OUTSIDE_CATACOMBS,
    STATE_MAZE_ONE,
    STATE_MAZE_TWO,
    STATE_REAR_GATE,
    STATE_WEST_CORRIDOR,
    STATE_LIBRARY_DESTROYED,
    STATE_STABLES,
    STATE_KEYSTONE_MAZE,
    STATE_GREENHOUSE,
    STATE_CHAIN_ROOM,
    STATE_THE_HATCH,
    /* (no ASAG ARENA row — see level_names) */
};

/* ---- Chapter headings in the level column ---------------------------------
   `first` is the index in level_names of the chapter's first room; the chapter
   runs to the room before the next chapter's first, so the list only has to
   stay in chapter order (it already is) for the headings to fall in the right
   places. Adding a room in the middle of a chapter needs nothing here; adding
   one at the head of a chapter means bumping the `first` below it.

   The split is by position in this list rather than by anything the rooms know
   about themselves because it does not follow the geography: WEST CORRIDOR is
   a house room and sits with the garden rooms of chapter two. */
typedef struct {
    int         first;
    const char *name;
} DebugChapter;

static const DebugChapter debug_chapters[] = {
    {  0, "CHAPTER 1" },
    { 14, "CHAPTER 2" },   /* FOUNTAIN SQR onwards, WEST CORRIDOR included */
};
#define DEBUG_CHAPTER_COUNT \
    ((int)(sizeof(debug_chapters) / sizeof(debug_chapters[0])))

/* The rule drawn under each heading, and the rows a heading costs: the name and
   its rule, plus a blank spacer row for every chapter after the first (the
   first sits directly under "LEVEL SELECT" and needs no gap). */
#define DBG_CHAPTER_RULE  "---------"
#define DBG_CHAPTER_ROWS(c)  ((c) ? 3 : 2)

/* Index of the room one past the end of chapter c. */
static int debug_chapter_end(int c) {
    return (c + 1 < DEBUG_CHAPTER_COUNT) ? debug_chapters[c + 1].first
                                         : LEVEL_SELECT_COUNT;
}

/* Row of the drawn list a room sits on, counting the headings — the cursor
   indexes ROOMS, the scroll window counts ROWS, and this is the bridge. */
static int debug_level_row(int lvl) {
    int row = 0, c;
    for (c = 0; c < DEBUG_CHAPTER_COUNT; c++) {
        int end = debug_chapter_end(c);
        row += DBG_CHAPTER_ROWS(c);
        if (lvl < end) return row + (lvl - debug_chapters[c].first);
        row += end - debug_chapters[c].first;
    }
    return row;
}

/* Chapter the cursor is in, so a room at the head of one can drag its heading
   into view with it rather than sitting on the top line orphaned. */
static int debug_level_chapter(int lvl) {
    int c;
    for (c = DEBUG_CHAPTER_COUNT - 1; c > 0; c--)
        if (lvl >= debug_chapters[c].first) return c;
    return 0;
}

/* Pull the scroll window onto the cursor: the fewest rows that puts it back on
   screen, so the list only moves when it has to. Called on every cursor move
   and when the menu opens. */
static void debug_scroll_follow(void) {
    int row   = debug_level_row(debug_menu_cursor);
    int total = debug_level_row(LEVEL_SELECT_COUNT - 1) + 1;
    int c     = debug_level_chapter(debug_menu_cursor);
    int top   = row;

    /* Stepping onto a chapter's first room shows that chapter's heading too. */
    if (debug_menu_cursor == debug_chapters[c].first)
        top -= DBG_CHAPTER_ROWS(c);

    if (top < debug_scroll)                     debug_scroll = top;
    if (row >= debug_scroll + DBG_LIST_ROWS)    debug_scroll = row - DBG_LIST_ROWS + 1;
    if (debug_scroll > total - DBG_LIST_ROWS)   debug_scroll = total - DBG_LIST_ROWS;
    if (debug_scroll < 0)                       debug_scroll = 0;
}

/* ---- Letter bitmasks: 7 rows x 5 cols, row 0 = top ---- */

static const uint8_t LETTER_H[7][5] = {
    {1,0,0,0,1},
    {1,0,0,0,1},
    {1,0,0,0,1},
    {1,1,1,1,1},
    {1,0,0,0,1},
    {1,0,0,0,1},
    {1,0,0,0,1},
};

static const uint8_t LETTER_O[7][5] = {
    {0,1,1,1,0},
    {1,0,0,0,1},
    {1,0,0,0,1},
    {1,0,0,0,1},
    {1,0,0,0,1},
    {1,0,0,0,1},
    {0,1,1,1,0},
};

static const uint8_t LETTER_R[7][5] = {
    {1,1,1,1,0},
    {1,0,0,0,1},
    {1,0,0,0,1},
    {1,1,1,1,0},
    {1,0,1,0,0},
    {1,0,0,1,0},
    {1,0,0,0,1},
};

static const uint8_t LETTER_P[7][5] = {
    {1,1,1,1,0},
    {1,0,0,0,1},
    {1,0,0,0,1},
    {1,1,1,1,0},
    {1,0,0,0,0},
    {1,0,0,0,0},
    {1,0,0,0,0},
};

static const uint8_t LETTER_E[7][5] = {
    {1,1,1,1,1},
    {1,0,0,0,0},
    {1,0,0,0,0},
    {1,1,1,1,0},
    {1,0,0,0,0},
    {1,0,0,0,0},
    {1,1,1,1,1},
};

static const uint8_t LETTER_S[7][5] = {
    {0,1,1,1,1},
    {1,0,0,0,0},
    {1,0,0,0,0},
    {0,1,1,1,0},
    {0,0,0,0,1},
    {0,0,0,0,1},
    {1,1,1,1,0},
};

static const uint8_t LETTER_T[7][5] = {
    {1,1,1,1,1},
    {0,0,1,0,0},
    {0,0,1,0,0},
    {0,0,1,0,0},
    {0,0,1,0,0},
    {0,0,1,0,0},
    {0,0,1,0,0},
};

static const uint8_t LETTER_A[7][5] = {
    {0,1,1,1,0},
    {1,0,0,0,1},
    {1,0,0,0,1},
    {1,1,1,1,1},
    {1,0,0,0,1},
    {1,0,0,0,1},
    {1,0,0,0,1},
};

static const uint8_t LETTER_SPACE[7][5] = {
    {0,0,0,0,0},{0,0,0,0,0},{0,0,0,0,0},{0,0,0,0,0},
    {0,0,0,0,0},{0,0,0,0,0},{0,0,0,0,0},
};

static const uint8_t LETTER_L[7][5] = {
    {1,0,0,0,0},
    {1,0,0,0,0},
    {1,0,0,0,0},
    {1,0,0,0,0},
    {1,0,0,0,0},
    {1,0,0,0,0},
    {1,1,1,1,1},
};

static const uint8_t LETTER_D[7][5] = {
    {1,1,1,1,0},
    {1,0,0,0,1},
    {1,0,0,0,1},
    {1,0,0,0,1},
    {1,0,0,0,1},
    {1,0,0,0,1},
    {1,1,1,1,0},
};

static const uint8_t LETTER_I[7][5] = {
    {1,1,1,1,1},
    {0,0,1,0,0},
    {0,0,1,0,0},
    {0,0,1,0,0},
    {0,0,1,0,0},
    {0,0,1,0,0},
    {1,1,1,1,1},
};

static const uint8_t LETTER_N[7][5] = {
    {1,0,0,0,1},
    {1,1,0,0,1},
    {1,0,1,0,1},
    {1,0,0,1,1},
    {1,0,0,0,1},
    {1,0,0,0,1},
    {1,0,0,0,1},
};

static const uint8_t LETTER_F[7][5] = {
    {1,1,1,1,1},
    {1,0,0,0,0},
    {1,0,0,0,0},
    {1,1,1,1,0},
    {1,0,0,0,0},
    {1,0,0,0,0},
    {1,0,0,0,0},
};

static const uint8_t LETTER_U[7][5] = {
    {1,0,0,0,1},
    {1,0,0,0,1},
    {1,0,0,0,1},
    {1,0,0,0,1},
    {1,0,0,0,1},
    {1,0,0,0,1},
    {0,1,1,1,0},
};

static const uint8_t LETTER_G[7][5] = {
    {0,1,1,1,1},
    {1,0,0,0,0},
    {1,0,0,0,0},
    {1,0,1,1,1},
    {1,0,0,0,1},
    {1,0,0,0,1},
    {0,1,1,1,1},
};

static const uint8_t LETTER_DOT[7][5] = {
    {0,0,0,0,0},
    {0,0,0,0,0},
    {0,0,0,0,0},
    {0,0,0,0,0},
    {0,0,0,0,0},
    {0,1,1,0,0},
    {0,1,1,0,0},
};

/* ---- Helpers ---- */

typedef const uint8_t (*LetterPtr)[5];

static void draw_letter(
    RenderContext *ctx,
    LetterPtr letter,
    int32_t sx, int32_t sy,
    int32_t tile_size,
    uint8_t r, uint8_t g, uint8_t b
) {
    int row, col;
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;

    for (row = 0; row < 7; row++) {
        for (col = 0; col < 5; col++) {
            if (!letter[row][col]) continue;
            if (ctx->next_packet + sizeof(TILE) > buf_end) return;

            TILE *tile = (TILE *)ctx->next_packet;
            setTile(tile);
            setXY0(tile, sx + col * tile_size, sy + row * tile_size);
            setWH(tile, tile_size - 1, tile_size - 1);
            setRGB0(tile, r, g, b);
            addPrim(&ctx->buffers[ctx->active_buffer].ot[1], tile);
            ctx->next_packet += sizeof(TILE);
        }
    }
}

static LetterPtr char_to_letter(char c) {
    switch (c) {
        case 'H': return LETTER_H;
        case 'O': return LETTER_O;
        case 'R': return LETTER_R;
        case 'P': return LETTER_P;
        case 'E': return LETTER_E;
        case 'S': return LETTER_S;
        case 'T': return LETTER_T;
        case 'A': return LETTER_A;
        case 'L': return LETTER_L;
        case 'D': return LETTER_D;
        case 'I': return LETTER_I;
        case 'N': return LETTER_N;
        case 'G': return LETTER_G;
        case 'F': return LETTER_F;
        case 'U': return LETTER_U;
        case '.': return LETTER_DOT;
        default:  return LETTER_SPACE;
    }
}

static void draw_title_string(
    RenderContext *ctx,
    const char    *str,
    int32_t        sx, int32_t sy,
    int32_t        tile_size,
    uint8_t r, uint8_t g, uint8_t b
) {
    int32_t letter_width = 5 * tile_size + tile_size;
    int i = 0;
    while (str[i]) {
        draw_letter(ctx, char_to_letter(str[i]),
                    sx + i * letter_width, sy,
                    tile_size, r, g, b);
        i++;
    }
}

static void draw_press_start(RenderContext *ctx) {
    static const LetterPtr letters[11] = {
        LETTER_P, LETTER_R, LETTER_E, LETTER_S, LETTER_S,
        LETTER_SPACE,
        LETTER_S, LETTER_T, LETTER_A, LETTER_R, LETTER_T,
    };

    /* Well under the title's tile size: this is the prompt, not the name of the
       game. It sits in the same band the New Game / Load Game menu takes over
       (see TMENU_Y), so pressing Start does not shift the screen around. */
    int32_t tile_size    = 2;
    int32_t letter_width = 5 * tile_size + tile_size;   /* 12  */
    int32_t total_width  = letter_width * 11;            /* 132 */
    int32_t start_x      = (SCREEN_XRES - total_width) / 2;
    int32_t start_y      = 152;
    int i;

    for (i = 0; i < 11; i++) {
        draw_letter(ctx, letters[i],
                    start_x + i * letter_width, start_y,
                    tile_size, 200, 200, 200);
    }
}

/* ---- Bottom-corner captions ------------------------------------------------
   Build stamp on the left, credit on the right, both on the same baseline in
   the 5x7 screen font (door_draw_string_2d). At 6px per cell the pair is 186 +
   90 = 276px of the 320 available, which the 8px debug font could not manage;
   TITLE_CAPTION_Y keeps them inside the overscan-safe area. Dim grey so they
   read as a footnote under the pulsing logo. */
#define TITLE_CAPTION_Y  224
#define TITLE_CAPTION_X    8   /* also the right margin, mirrored */

static const char TITLE_CAPTION_LEFT[]  = "Preview Version";
static const char TITLE_CAPTION_RIGHT[] = "Created by @electricreload 2026";

static void draw_title_captions(RenderContext *ctx) {
    door_draw_string_2d(ctx, TITLE_CAPTION_LEFT,
                        TITLE_CAPTION_X, TITLE_CAPTION_Y, 150, 150, 150, 1);
    door_draw_string_2d(ctx, TITLE_CAPTION_RIGHT,
                        SCREEN_XRES - TITLE_CAPTION_X
                            - door_small_text_width(TITLE_CAPTION_RIGHT),
                        TITLE_CAPTION_Y, 150, 150, 150, 1);
}

/* ---- Public functions ---- */

/* The game's title at its title-screen size and position. Shared by draw_title
   (which pulses it red) and by the opening sequence, which fades it to black.

   One word per line, so it is the seven letters of NINURTA — not all sixteen
   characters — that have to fit across. A cell costs 6 * tile_size wide and a
   line 8 * tile_size tall (7 glyph rows + a blank row between lines), giving
   7 * 6 * 5 = 210 wide and 3 * 8 * 5 - 5 = 115 tall at this size. The stack
   ends at y=133, which is what leaves the band below it free for the menu. */
static const char *const title_lines[3] = { "ORDER", "OF", "NINURTA" };

void title_draw_logo(RenderContext *ctx, uint8_t r, uint8_t g, uint8_t b) {
    int32_t tile_size    = 5;
    int32_t letter_width = 5 * tile_size + tile_size;   /* 30 */
    int32_t line_step    = 7 * tile_size + tile_size;   /* 40 */
    int32_t start_y      = 18;                          /* stack runs 18..133 */
    int i, len;

    /* Each line is centred on its own width, so the block reads as centred
       however the words are later reworded. */
    for (i = 0; i < 3; i++) {
        for (len = 0; title_lines[i][len]; len++) ;
        draw_title_string(ctx, title_lines[i],
                          (SCREEN_XRES - len * letter_width) / 2,
                          start_y + i * line_step,
                          tile_size, r, g, b);
    }
}

void draw_title(RenderContext *ctx) {
    static int32_t pulse      = 0;
    static int32_t title_flash = 0;

    pulse = (pulse + 2) & 255;
    uint8_t red = (uint8_t)(180 + ((pulse < 128) ? pulse / 2 : (255 - pulse) / 2));

    /* The debug level select takes over the whole screen — hide the title
       while it is open; it returns when Select backs out to PRESS START. */
    if (!debug_menu_open)
        title_draw_logo(ctx, red, 0, 0);

    /* Only the debug menu covers the screen; every menu state leaves the bottom
       corners free now that they all share one prompt row at TMENU_PROMPT_Y. */
    if (!debug_menu_open)
        draw_title_captions(ctx);

    if (debug_menu_open) {
        int k, c, row = 0;
        /* Left column: rooms under their chapter headings, windowed to the rows
           that fit above the footer (see DBG_LIST_ROWS). Only the rows inside
           the window are printed at all — FntPrint advances a line per string
           printed, so skipping the rest is what does the scrolling, and it
           keeps the stream's per-frame character budget down as a bonus.
           The cursor only shows while this column is active, so it is always
           clear which room Circle will act on. */
#define DBG_ROW_SHOWN(r)  ((r) >= debug_scroll && (r) < debug_scroll + DBG_LIST_ROWS)
        FntPrint(debug_fnt, "LEVEL SELECT\n\n");
        for (c = 0; c < DEBUG_CHAPTER_COUNT; c++) {
            int end = debug_chapter_end(c);
            if (c) {
                if (DBG_ROW_SHOWN(row)) FntPrint(debug_fnt, "\n");
                row++;
            }
            if (DBG_ROW_SHOWN(row)) FntPrint(debug_fnt, "%s\n", debug_chapters[c].name);
            row++;
            if (DBG_ROW_SHOWN(row)) FntPrint(debug_fnt, "%s\n", DBG_CHAPTER_RULE);
            row++;
            for (k = debug_chapters[c].first; k < end; k++, row++) {
                if (!DBG_ROW_SHOWN(row)) continue;
                FntPrint(debug_fnt, "%s %s\n",
                         (debug_col == DBG_COL_LEVELS && k == debug_menu_cursor)
                             ? "*" : " ",
                         level_names[k]);
            }
        }
#undef DBG_ROW_SHOWN
        FntFlush(debug_fnt);

        /* Right column: cheat toggles, as "*[X] NAME" — cursor, then a checkbox
           carrying the on/off state. Drawn piece by piece at fixed columns (no
           string building, and no font stream — see the cap noted above); the
           8px advance matches the left column's line height. */
        btn_prompt_draw(ctx, DBG_OPT_X, 8, "OPTIONS", 1);
        for (k = 0; k < DEBUG_OPT_COUNT; k++) {
            int oy = DBG_OPT_TOP_Y + k * 8;
            if (debug_col == DBG_COL_OPTS && k == debug_opt_cursor)
                btn_prompt_draw(ctx, DBG_OPT_X, oy, "*", 1);
            btn_prompt_draw(ctx, DBG_OPT_BOX_X,  oy, debug_opts[k] ? "[X]" : "[ ]", 1);
            btn_prompt_draw(ctx, DBG_OPT_NAME_X, oy, debug_opt_names[k], 1);
        }

        /* Footer with the coloured button glyphs, along the bottom of the
           screen (the Fnt streams can't hold coloured glyphs, so it is drawn
           separately). Circle acts, Cross backs out — the same convention as
           the start menu and the in-game save menu. */
        btn_prompt_draw(ctx, 8, DBG_FOOTER_Y,
                        BTN_CIRCLE ":LOAD/TOGGLE  " BTN_CROSS ":BACK", 1);
    } else if (tmenu == TM_MAIN) {
        /* 12 characters wide with both dashes; 4 spaces put them at x=112..208,
           dead centre. */
        FntPrint(level_select_fnt, "    %s NEW GAME %s\n    %s LOAD GAME %s\n",
                 TMENU_MARK(tmenu_cursor == 0), TMENU_MARK(tmenu_cursor == 0),
                 TMENU_MARK(tmenu_cursor == 1), TMENU_MARK(tmenu_cursor == 1));
        FntFlush(level_select_fnt);
    } else if (tmenu == TM_CARD) {
        /* Heading is 9 wide and the card lines 17, so they centre at 5 and 1
           spaces respectively (each a half-character shy of exact). */
        FntPrint(level_select_fnt, "     LOAD GAME\n\n");
        FntPrint(level_select_fnt, " %s MEMORY CARD 1 %s\n %s MEMORY CARD 2 %s\n",
                 TMENU_MARK(tmenu_cursor == 0), TMENU_MARK(tmenu_cursor == 0),
                 TMENU_MARK(tmenu_cursor == 1), TMENU_MARK(tmenu_cursor == 1));
        /* The error line's length varies, so it takes a fixed indent rather
           than a centring pad. It sits on the row below the second card. */
        if (tmenu_msg)
            FntPrint(level_select_fnt, "   %s\n", tmenu_msg);
        FntFlush(level_select_fnt);
    } else if (tmenu == TM_FILE) {
        /* Same heading row as TM_CARD, then as many save rows as fit above the
           prompt (see TMENU_FILE_ROWS), scrolled so the cursor is always in
           view. An error line borrows the last row rather than being drawn on
           top of the prompt, so a full list shows one entry fewer while it is
           up. Titles run to 32 characters, hence the wider window. */
        int fnt   = load_list_fnt >= 0 ? load_list_fnt : level_select_fnt;
        int rows  = tmenu_msg ? TMENU_FILE_ROWS - 1 : TMENU_FILE_ROWS;
        int first = (tmenu_cursor >= rows) ? tmenu_cursor - rows + 1 : 0;
        int k;
        /* 11 characters, 13 spaces in from x=8: centred on 160 like LOAD GAME. */
        FntPrint(fnt, "             SELECT SAVE\n\n");
        for (k = first; k < tmenu_slot_count && k < first + rows; k++)
            FntPrint(fnt, "%s %s %s\n",
                     TMENU_MARK(k == tmenu_cursor), tmenu_slots[k].title,
                     TMENU_MARK(k == tmenu_cursor));
        if (tmenu_msg)
            FntPrint(fnt, "%s\n", tmenu_msg);
        FntFlush(fnt);
    } else {
        title_flash++;
        if ((title_flash & 63) < 40)
            draw_press_start(ctx);
    }

    /* Drawn once here rather than per state so all three menus show the same
       prompt in the same place, in the same font as the debug menu's footer. */
    if (!debug_menu_open && tmenu != TM_CLOSED)
        btn_prompt_draw(ctx, TMENU_PROMPT_X, TMENU_PROMPT_Y, TMENU_PROMPT, 1);
}

void title_init(void) {
    /* Font window for the start menu, in the band below the title stack — see
       the TMENU_* notes above. 176px is 22 characters, wide enough for the
       memory-card lines and the longest error message, and the height stops it
       short of the prompt row. */
    level_select_fnt = FntOpen(TMENU_X, TMENU_Y, 176, 72, 0, 256);
    /* The save-file list starts on the same row so its heading lines up with
       TM_CARD's, but needs a wider window (32-char titles + a dash and a space
       on each side is 36 characters, so 304px / 38 characters from x=8). */
    load_list_fnt = FntOpen(8, TMENU_Y, 304, 80, 0, 512);
    /* Debug menu's LEFT column only — 128px is 16 characters, and "* MASTER
       BEDROOM" is exactly 16. The right column needs no stream (see the eight-
       stream cap documented at the top of this file). This is the eighth and
       last FntOpen in the project.

       The last argument is the CHARACTER budget for one frame's FntPrints, and
       FntPrint silently drops everything past it — the symptom is the tail of
       the list vanishing, with the row that straddles the limit printed
       half-finished ("MAZE ONE" came out as "MAZE ON"). The header plus a row
       per level costs 14 + sum(strlen(name) + 3), which was 268 at eighteen
       rooms and had already overrun 256. 512 leaves room for roughly sixteen
       more entries; raise it again when the list next grows. */
    debug_fnt = FntOpen(8, 8, 128, 224, 0, 512);
}

/* ---- Held-direction auto-repeat -------------------------------------------
   Every menu on this screen navigates with the d-pad, and a long list (the
   debug room column especially) is miserable to walk one full press at a time.
   A held direction therefore repeats: NAV_DELAY frames after the press, then
   one step every NAV_RATE frames for as long as it is held. The confirm and
   back buttons deliberately do NOT repeat — a held Circle must not fire twice.

   Fresh presses still come through the moment they happen, so a quick tap is
   exactly as responsive as it was; the repeat only ever ADDS steps. */
#define NAV_DELAY  14   /* frames held before the first repeat  */
#define NAV_RATE    4   /* frames between repeats after that    */
#define NAV_DIRS   (PAD_UP | PAD_DOWN | PAD_LEFT | PAD_RIGHT)

/* Returns the directions that should act THIS frame: the newly pressed ones,
   plus the held ones once their timer comes round. */
static uint16_t nav_repeat(uint16_t held, uint16_t pressed) {
    static int timer = 0;
    uint16_t dirs = held & NAV_DIRS;
    uint16_t fresh = pressed & NAV_DIRS;

    if (!dirs) { timer = 0; return 0; }
    /* A new direction restarts the wait, so rolling from one to another does
       not inherit the previous one's part-spent timer. */
    if (fresh) { timer = NAV_DELAY; return fresh; }
    if (--timer <= 0) { timer = NAV_RATE; return dirs; }
    return 0;
}

void update_title(void) {
    if (!pad_buff_len[0]) return;
    PadResponse *pad = (PadResponse *)pad_buff[0];

    /* Edge-detect newly pressed buttons (pad bits are active-low). */
    static uint16_t prev_held = 0;
    uint16_t held    = ~pad->btn;
    uint16_t pressed = held & ~prev_held;
    prev_held = held;

    /* Directions go through the auto-repeat; everything else stays edge-only.
       Called once, unconditionally, so the timer keeps ticking whichever menu
       state is up and cannot be left part-wound by a state change. */
    uint16_t nav = nav_repeat(held, pressed);

    if (!debug_menu_open && tmenu == TM_CLOSED) {
        if (pressed & PAD_SELECT) {
            debug_menu_open   = 1;
            debug_menu_cursor = 0;
            debug_scroll      = 0;
            debug_scroll_follow();
            sound_play(SFX_SELECT);
        } else if (pressed & PAD_START) {
            tmenu        = TM_MAIN;   /* New Game / Load Game */
            tmenu_cursor = 0;
            tmenu_msg    = 0;
            sound_play(SFX_SELECT);
        }
        return;
    }

    if (debug_menu_open) {
        /* Up/Down moves within the active column, Left/Right switches column,
           Circle loads (levels) or flips the toggle (options), Cross backs out.
           Select still closes the menu as well — it is the key that opened it. */
        /* One cursor blip for any move, whichever column it happens in — a
           column switch reads as a cursor move to the player exactly as an
           up/down step does. */
        if ((nav & (PAD_UP | PAD_DOWN)) || (pressed & (PAD_LEFT | PAD_RIGHT)))
            sound_play(SFX_CURSOR);

        /* Only UP/DOWN auto-repeats here. Left/Right is a two-way switch, so a
           held direction would flap between the columns several times a second
           instead of settling on the one being pointed at. */
        if (pressed & (PAD_LEFT | PAD_RIGHT))
            debug_col = (debug_col == DBG_COL_LEVELS) ? DBG_COL_OPTS : DBG_COL_LEVELS;

        if (debug_col == DBG_COL_LEVELS) {
            if (nav & PAD_UP)
                debug_menu_cursor = (debug_menu_cursor + LEVEL_SELECT_COUNT - 1) % LEVEL_SELECT_COUNT;
            if (nav & PAD_DOWN)
                debug_menu_cursor = (debug_menu_cursor + 1) % LEVEL_SELECT_COUNT;
            /* After every move, not only the ones that leave the window: a wrap
               round either end of the list jumps the window the whole way. */
            debug_scroll_follow();
        } else {
            if (nav & PAD_UP)
                debug_opt_cursor = (debug_opt_cursor + DEBUG_OPT_COUNT - 1) % DEBUG_OPT_COUNT;
            if (nav & PAD_DOWN)
                debug_opt_cursor = (debug_opt_cursor + 1) % DEBUG_OPT_COUNT;
        }

        if (pressed & (PAD_SELECT | PAD_CROSS)) {
            debug_menu_open = 0;
            sound_play(SFX_BACK);
        }
        if (pressed & PAD_CIRCLE) {
            sound_play(SFX_SELECT);
            if (debug_col == DBG_COL_OPTS) {
                /* Toggling leaves the menu open so several can be set in a row. */
                debug_opts[debug_opt_cursor] = !debug_opts[debug_opt_cursor];
            } else {
                debug_menu_open = 0;
                GameState target = level_states[debug_menu_cursor];
                /* STATE_LOADING entries (kitchen, reception) need the area to switch to. */
                if (target == STATE_LOADING) pending_area = level_pending[debug_menu_cursor];
                /* The inventory cheats can't be handed out yet — the destination
                   room hasn't initialised. Arm them; main.c applies them once it
                   has, alongside a staged save's player state. */
                debug_opts_arm_grants();
                game_state = target;
            }
        }
        return;
    }

    /* ---- Start menu ----
       Circle = select, X = back: matches the in-game save menu's convention. */
    int confirm = (pressed & PAD_CIRCLE) ? 1 : 0;
    int back    = (pressed & PAD_CROSS)  ? 1 : 0;

    /* One place for all three states' blips. SELECT fires on every confirm the
       menu acts on, including the ones that end in an inline error (NO MEMORY
       CARD, LOAD FAILED): the press was registered and the player needs to hear
       that, and the message on screen is what says it did not work. */
    /* The save list is the only start-menu state long enough to be worth
       holding a direction on; the other two are two-way toggles that would
       flap under auto-repeat, so they stay on the fresh press. */
    if (((tmenu == TM_FILE ? nav : pressed) & (PAD_UP | PAD_DOWN)))
        sound_play(SFX_CURSOR);
    if (back)    sound_play(SFX_BACK);
    if (confirm) sound_play(SFX_SELECT);

    if (tmenu == TM_MAIN) {
        if (pressed & (PAD_UP | PAD_DOWN)) tmenu_cursor ^= 1;
        if (back) { tmenu = TM_CLOSED; return; }
        if (confirm) {
            if (tmenu_cursor == 0) {
                /* New Game runs the opening sequence first; it hands over to
                   the delivery area itself once it finishes (or is skipped).
                   Closing the menu here is what makes the rest of the title's
                   text vanish immediately — draw_title stops being called on
                   the very next frame, so nothing of it is left to fade. */
                tmenu      = TM_CLOSED;
                intro_start();
                game_state = STATE_INTRO;
            } else {
                tmenu        = TM_CARD;             /* Load Game */
                tmenu_cursor = 0;
                tmenu_msg    = 0;
            }
        }
    } else if (tmenu == TM_CARD) {
        if (pressed & (PAD_UP | PAD_DOWN)) { tmenu_cursor ^= 1; tmenu_msg = 0; }
        if (back) { tmenu = TM_MAIN; tmenu_cursor = 1; tmenu_msg = 0; return; }
        if (confirm) {
            tmenu_port = tmenu_cursor;
            memcard_begin();
            int present = memcard_present(tmenu_port);
            memcard_end();
            if (!present) { tmenu_msg = "NO MEMORY CARD"; return; }
            int n = savegame_list(tmenu_port, tmenu_slots, SAVE_MAX_SLOTS);
            if (n < 0)       { tmenu_msg = "CARD READ ERROR"; return; }
            if (n == 0)      { tmenu_msg = "NO SAVES ON CARD"; return; }
            tmenu_slot_count = n;
            tmenu        = TM_FILE;
            tmenu_cursor = 0;
            tmenu_msg    = 0;
        }
    } else if (tmenu == TM_FILE) {
        if (nav & PAD_UP)
            tmenu_cursor = (tmenu_cursor + tmenu_slot_count - 1) % tmenu_slot_count;
        if (nav & PAD_DOWN)
            tmenu_cursor = (tmenu_cursor + 1) % tmenu_slot_count;
        if (back) { tmenu = TM_CARD; tmenu_cursor = tmenu_port; tmenu_msg = 0; return; }
        if (confirm) {
            SaveData sd;
            if (savegame_read(tmenu_port, tmenu_slots[tmenu_cursor].block, &sd) != MC_OK) {
                tmenu_msg = "LOAD FAILED";
                return;
            }
            /* Stage the state for main.c to apply once the area is set up, then
               route into the saved area exactly like the level select. */
            savegame_stage_load(&sd);
            tmenu = TM_CLOSED;
            /* The delivery area is entered directly; every other area is set up
               by STATE_LOADING (kitchen, reception, piano room, conservatory). */
            if (sd.area == (int32_t)STATE_DELIVERY_AREA) {
                game_state = STATE_DELIVERY_AREA;
            } else {
                pending_area = (GameState)sd.area;
                game_state   = STATE_LOADING;
            }
        }
    }
}

/* ---- The loading screen's slashing crucifaxe ------------------------------
   The red LOADING screen used to be completely static apart from three dots,
   which on a long room build reads as a hung console rather than a busy one.
   The axe gives it constant motion: it swings down and back on the spot while
   travelling from one side of the screen to the other, then flips and comes
   back the other way.

   >>> THIS ONLY MOVES IF SOMEBODY PUMPS IT. <<< A load is one long BLOCKING
   pass inside a single frame (main.c's STATE_LOADING), so nothing on screen
   changes for its whole duration unless the loader stops between steps to draw
   and flip. main.c's loading_screen_pump() is that stop; the animation state
   below advances once per draw, so the number of frames the player sees is
   exactly the number of pump calls. Adding a step to a load without pumping
   after it simply freezes the axe for that step's duration.

   The icon is the menu's CRFXICON.TIM, loaded again here rather than borrowed
   from menu.c: menu_init() runs at the END of main()'s startup block, and the
   longest freeze in the game is that block itself. Loading it first thing means
   the axe is already on screen for it. The second LoadImage in menu_init hits
   the same VRAM address with the same pixels, so the two copies cost a CD read,
   not VRAM. */

static uint16_t axe_tpage = 0;   /* 0 = not loaded; the loading screen skips it */
static uint16_t axe_clut  = 0;
static uint8_t  axe_u0 = 0, axe_v0 = 0, axe_u1 = 0, axe_v1 = 0;

/* The spin rate is per DRAWN FRAME, and a load draws nowhere near sixty of
   those a second — it draws one per loading_screen_pump() call, which is one
   per step of the load. It is therefore tuned against the number of pumps a
   load makes (a few dozen), not against a frame rate: at 30 degrees a frame the
   axe turns a full circle every twelve steps. Slower and it reads as a still
   image with a twitch in it, which is the thing this is here to avoid. */
#define AXE_SIZE     24   /* on-screen square, in pixels                     */
#define AXE_MARGIN   16   /* gap kept between the icon and the screen edges  */
#define AXE_SPIN_DEG 30   /* degrees turned per drawn frame                  */

void loading_screen_load_axe(void) {
    CdlFILE file;
    if (!CdSearchFile(&file, "\\CRFXICON.TIM;1")) return;
    int   sectors = (file.size + 2047) / 2048;
    void *buf     = malloc(sectors * 2048);
    if (!buf) return;
    CdControl(CdlSetloc, &file.pos, NULL);
    CdRead(sectors, (uint32_t *)buf, CdlModeSpeed);
    CdReadSync(0, NULL);

    TIM_IMAGE tim;
    GetTimInfo((uint32_t *)buf, &tim);
    LoadImage(tim.prect, tim.paddr);
    DrawSync(0);
    if (tim.mode & 0x8) {
        LoadImage(tim.crect, tim.caddr);
        DrawSync(0);
        axe_clut = getClut(tim.crect->x, tim.crect->y);
    }

    /* UV within the tpage - same derivation as menu.c's load_icon_tim. */
    int bpp_mode = tim.mode & 3;
    int px_mult  = (bpp_mode == 0) ? 4 : (bpp_mode == 1) ? 2 : 1;
    int u_off    = (tim.prect->x & 63) * px_mult;
    axe_u0 = (uint8_t)u_off;
    axe_v0 = (uint8_t)(tim.prect->y % 256);
    axe_u1 = (uint8_t)(u_off + tim.prect->w * px_mult - 1);
    axe_v1 = (uint8_t)(axe_v0 + tim.prect->h - 1);

    axe_tpage = getTPage(bpp_mode, 0, tim.prect->x, tim.prect->y);
    free(buf);
}

/* One frame of the axe: advance the walk and the swing, then sort a rotated
   POLY_FT4 for it. Called from draw_loading_screen only. */
static void draw_loading_axe(RenderContext *ctx) {
    if (!axe_tpage) return;

    /* Parked in the bottom-left corner, one margin in from both edges. It does
       not travel: the spin alone is the "something is still happening" signal,
       and a fixed position keeps it out of the way of the word above it. */
    const int32_t half = AXE_SIZE / 2;
    const int32_t cx   = AXE_MARGIN + half;
    const int32_t cy   = 240 - AXE_MARGIN - half;

    /* Turned a fixed amount every drawn frame, wrapping at a full circle.
       Measured in ONE units (4096 = 360 degrees) rather than degrees so the
       wrap is exact and nothing accumulates rounding. */
    static int32_t ang = 0;
    ang = (ang + (AXE_SPIN_DEG * ONE) / 360) & (ONE - 1);
    int32_t sn = isin(ang), cs = icos(ang);

    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    if (ctx->next_packet + sizeof(POLY_FT4) + sizeof(DR_TWIN) > buf_end) return;

    POLY_FT4 *poly = (POLY_FT4 *)ctx->next_packet;
    setPolyFT4(poly);
    setRGB0(poly, 255, 255, 255);  /* dark source art, drawn at 2x as the HUD's is */

    /* Corners TL, TR, BL, BR, each rotated about the centre - which is the
       centre of the icon's own square, so the axe turns on the spot. The quad
       is the whole trick: the GPU shears the texture to fit whatever four
       corners it is given. */
    int32_t cos_h = (cs * half) >> 12;   /* half-diagonal components, once */
    int32_t sin_h = (sn * half) >> 12;
    setXY4(poly,
           cx - cos_h + sin_h, cy - sin_h - cos_h,   /* top-left     */
           cx + cos_h + sin_h, cy + sin_h - cos_h,   /* top-right    */
           cx - cos_h - sin_h, cy - sin_h + cos_h,   /* bottom-left  */
           cx + cos_h - sin_h, cy + sin_h + cos_h);  /* bottom-right */

    poly->u0 = axe_u0;  poly->v0 = axe_v0;
    poly->u1 = axe_u1;  poly->v1 = axe_v0;
    poly->u2 = axe_u0;  poly->v2 = axe_v1;
    poly->u3 = axe_u1;  poly->v3 = axe_v1;

    poly->tpage = axe_tpage;
    poly->clut  = axe_clut;
    addPrim(&ctx->buffers[ctx->active_buffer].ot[2], poly);
    ctx->next_packet += sizeof(POLY_FT4);

    /* Reset the texture window before the icon, for the reason menu.c does it:
       a room that sorted a 128x128 window at the top of the OT is still active
       here, and this icon's texture sits at V offset 128 within its page - with
       the window left in place it samples the texture above it instead.
       RECT{0,0,0,0} = full page. Sorted BEHIND the icon (higher OT index) so the
       GPU applies it before the icon is drawn. */
    {
        RECT full = {0, 0, 0, 0};
        DR_TWIN *tw = (DR_TWIN *)ctx->next_packet;
        setTexWindow(tw, &full);
        addPrim(&ctx->buffers[ctx->active_buffer].ot[3], tw);
        ctx->next_packet += sizeof(DR_TWIN);
    }
}

void draw_loading_screen(RenderContext *ctx) {
    /* Full-screen red background */
    TILE *bg = (TILE *)ctx->next_packet;
    setTile(bg);
    setXY0(bg, 0, 0);
    setWH(bg, 320, 240);
    setRGB0(bg, 120, 0, 0);
    addPrim(&ctx->buffers[ctx->active_buffer].ot[OT_LENGTH - 1], bg);
    ctx->next_packet += sizeof(TILE);

    int32_t tile_size    = 4;
    int32_t letter_width = 5 * tile_size + tile_size;  /* 24 */
    int32_t start_x      = (320 - (7 * letter_width)) / 2;   /* "LOADING" = 7 */
    int32_t start_y      = 100;
    draw_title_string(ctx, "LOADING", start_x, start_y, tile_size, 255, 255, 255);

    /* NO ANIMATED DOTS. There used to be three, cycling every thirty frames,
       and at tile_size 4 the title font draws each one as a plain white block.
       They never cycled either: a load draws one frame per step (see the pump
       note above), so thirty of them is longer than most loads and what the
       player actually saw was a couple of white squares sitting still under the
       word. The axe is the progress indicator now. */

    draw_loading_axe(ctx);
}
