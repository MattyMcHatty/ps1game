#ifndef WEAPON_H
#define WEAPON_H

#include <psxgte.h>
#include <smd/smd.h>
#include "render.h"

/*
 * Weapon layer: owns the equipped-weapon dispatch shared by every playable area.
 * Each concrete weapon (crucifaxe, grave-olver) provides its own init/draw (and,
 * later, its own update/fire); this module loads them all, cycles the equipped
 * one on L2, and routes update/draw to whichever is current (see player.h
 * current_weapon / player_weapons).
 */
void weapons_init(void);            /* load every weapon's model (startup) */
void weapons_update(void);          /* L2 cycle + the equipped weapon's update */
void weapons_draw(RenderContext *ctx);

/* Shared flat-shaded, view-space weapon-model renderer used by each weapon's
   draw. weapon_vs is the fully-built view-space transform for the model; the
   caller decides the hold pose / animation. `gain` scales the model's base
   colours (GTE fixed point, 4096 = 1.0x) before shading, so a model authored
   with dark base colours can be brightened without changing the shared shading.
   Restores the camera view matrix before returning. */
void weapon_render_model(RenderContext *ctx, SMD *smd, MATRIX *weapon_vs,
                         int32_t gain);

#endif
