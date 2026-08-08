#ifndef HUD_H
#define HUD_H

#include "render.h"

/*
 * HUD: the bottom-of-screen status panel, shared by every room.
 *
 * ART
 *   textures/hud.png is authored at 320x56 — exactly screen width — so it sits
 *   at screen y 184..239 at 1:1 with no scaling. Its four yellow-bordered boxes
 *   are, left to right: health bar, stamina bar (stacked on the left), the
 *   equipped-weapon box, and the log box. Everything except the log box's fill
 *   is transparent in the art, so the panel is a FRAME laid over live content.
 *
 *   VRAM: 4bpp at (384,384), CLUT (176,511). 320 px is 80 VRAM words, and a
 *   4bpp texture page is only 64 words (256 px) wide, so the panel STRADDLES a
 *   page boundary at word x=448 and is drawn as two abutting quads — screen
 *   x 0..255 from the page at 384, x 256..319 from the page at 448. This keeps
 *   it pixel-perfect; stretching a 256-wide copy up to 320 would smear the 2px
 *   yellow borders to an uneven 2-3px.
 *
 *   Its VRAM y of 384 means Voff 128 (see tools/VRAM_MAP.txt), and it is 320 px
 *   wide, so a live 128x128 texture window wraps BOTH its v and its u and the
 *   panel renders as stove.tim / prpl_wlppr.tim with din_cl.tim on the end —
 *   the textures 128 lines above it. Every HUD bucket that draws a texture
 *   therefore carries its own window disable IN THAT BUCKET. One reset sorted
 *   above the HUD is NOT enough and was the original bug; see hud.c's
 *   hud_disable_texwindow and tools/HUD_NOTES.txt PART 3b before changing it.
 *
 * DRAW ORDER (back to front, all inside the menu-reserved OT block 0-15)
 *   purple backing bar -> health/stamina fills + weapon icon -> the panel
 *   texture -> ammo count -> log text.
 *
 * VISIBILITY — the HUD is hidden:
 *   1. while a camera-locked puzzle or a boss cutscene owns the screen (the log
 *      box alone stays up via hud_draw_log_only, so puzzle log lines still read)
 *   2. on the title screen        (that state never calls a draw here)
 *   3. while the inventory or save menu is open
 *   4. during room transitions    (door anim / stair anim / loading)
 * Only 1 and 3 need code: main.c's other states never reach a HUD draw at all.
 */

/* Load HUD.TIM into VRAM. STARTUP ONLY — LoadImage is unsafe once the render
   loop is running (see tools/TEXTURING_NOTES.txt, PROBLEM A). */
void hud_load_texture(void);

/* Tick the log's message timers. Call once per frame from the area update, NOT
   from the draw: the log must expire on its own while the HUD is hidden, or a
   line posted during a puzzle would still be sitting there afterwards. */
void hud_log_update(void);

/* The whole panel: backing bar, bars, weapon + ammo, frame, log. */
void hud_draw(RenderContext *ctx);

/* The log box only, in its usual place — for camera-locked puzzles, which hide
   the rest of the HUD but still post log lines the player has to read. A no-op
   while the log is empty, so a boss cutscene (which posts nothing) shows no
   stray box over its reveal. */
void hud_draw_log_only(RenderContext *ctx);

#endif
