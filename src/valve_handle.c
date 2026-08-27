#include <stdint.h>
#include <stdlib.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <psxcd.h>
#include <inline_c.h>
#include <smd/smd.h>
#include "render.h"
#include "camera.h"
#include "tim_slots.h"
#include "title.h"
#include "valve_handle.h"

/* The room the player is in; mounts of other areas are skipped. NOT game_state
   — see the same note in src/fatdoor.c. */
extern GameState current_area;

ValveMount valve_mounts[MAX_VALVE_MOUNTS];
int        valve_mount_count = 0;

static ValveMount valve_defaults[MAX_VALVE_MOUNTS];
static SMD       *valve_smd    = NULL;
static void      *valve_buffer = NULL;

static void load_file(const char *name, void **buf_out) {
    CdlFILE file;
    if (!CdSearchFile(&file, (char *)name)) { *buf_out = NULL; return; }
    int sectors = (file.size + 2047) / 2048;
    *buf_out = malloc(sectors * 2048);
    if (!*buf_out) return;
    CdControl(CdlSetloc, &file.pos, NULL);
    CdRead(sectors, (uint32_t *)*buf_out, CdlModeSpeed);
    CdReadSync(0, NULL);
}

void valve_handles_load_assets(void) {
    load_file("\\TEX\\VALVEH.SMD;1", &valve_buffer);
    if (valve_buffer)
        valve_smd = smdInitData(valve_buffer);
}

/* WHICH PIPE TEXTURE THIS ROOM HAS IN VRAM. The mesh is linked against pipe.tim
   and that is the right answer in any room which draws pipe.tim at its own slot
   (x768 y0) — but the Greenhouse draws brick_wall from that page and puts the
   4bpp clone pipe_gh.tim up at x384 y256 instead, so there the handle has to be
   pointed at the clone. One line per room; the default is the original. */
static void valve_pipe_slot(GameState area, uint16_t *tpage, uint16_t *clut) {
    if (area == STATE_GREENHOUSE) {
        *tpage = TIM_TPAGE_PIPEGH;
        *clut  = TIM_CLUT_PIPEGH;
    } else {
        *tpage = TIM_TPAGE_PIPE;
        *clut  = TIM_CLUT_PIPE;
    }
}

void valve_handles_init(void) {
    int i = 0;

    /* GREENHOUSE, the standing pipe in the far south bay. The pipe is a 50x50
       column at x[-1460,-1410] z[-3825,-3775], and the BLACK HOLE in its texture
       falls on the +Z face — the one that looks back up the room — at
       (-1435, -197). Mapping it: the face is the +Z quad spanning y[-225,-167]
       (primitive 726 in the Aug 2026 decimated export, 919 in the one before it
       -- the INDEX moves with every re-export and the COORDINATES do not, so
       trust these rather than the number), with
       u 66..130 across x -1460..-1410 and v 122..203 across y -225..-166.84, and
       the hole's centre in pipe_128.png is texel (97.5, 33.4); 33.4 + 128 = 161.4
       lands inside that v span because the window wraps at 128.

       So the marker poly goes at (-1435, -197, -3775) and the model origin is one
       stem length in FRONT of it, at z = -3775 + 20. rot_x = -1024 turns the
       model's +Y (the stem) into world -Z, which points it into the pipe, and
       stands the wheel up in the XY plane facing back down the room.

       PRESENT at the start: this is where the player finds the handle. */
    valve_mounts[i].x = -1435; valve_mounts[i].y = -197;
    valve_mounts[i].z = -3775 + VALVE_STEM_LEN;
    valve_mounts[i].rot_x = -1024; valve_mounts[i].rot_y = 0;
    valve_mounts[i].present = 1;
    valve_mounts[i].spin = 0; valve_mounts[i].turn_timer = 0;
    valve_mounts[i].active = 1;
    valve_mounts[i].area = STATE_GREENHOUSE; i++;

    valve_mount_count = i;

    int j;
    for (j = 0; j < valve_mount_count; j++)
        valve_defaults[j] = valve_mounts[j];
}

void valve_handles_reset(void) {
    int i;
    for (i = 0; i < valve_mount_count; i++)
        valve_mounts[i] = valve_defaults[i];
}

int valve_mount_in_area(GameState area) {
    int i;
    for (i = 0; i < valve_mount_count; i++)
        if (valve_mounts[i].active && valve_mounts[i].area == area) return i;
    return -1;
}

void valve_handle_set_present(int mount, int present) {
    if (mount < 0 || mount >= valve_mount_count) return;
    valve_mounts[mount].present    = present ? 1 : 0;
    valve_mounts[mount].turn_timer = 0;      /* taking it off cancels a turn */
}

int valve_handle_present(int mount) {
    if (mount < 0 || mount >= valve_mount_count) return 0;
    return valve_mounts[mount].present;
}

void valve_handle_begin_turn(int mount) {
    if (mount < 0 || mount >= valve_mount_count) return;
    ValveMount *m = &valve_mounts[mount];
    if (!m->present || m->turn_timer > 0) return;
    m->turn_timer = VALVE_TURN_FRAMES;
}

int valve_handle_turning(int mount) {
    if (mount < 0 || mount >= valve_mount_count) return 0;
    return valve_mounts[mount].turn_timer > 0;
}

/* The whole animation. A turn is VALVE_TURN_REVS revolutions spread evenly over
   VALVE_TURN_FRAMES, and the angle is derived from the REMAINING frames rather
   than accumulated, so a turn always lands back on the angle it started from and
   rounding cannot walk the wheel off true over a long session.

   Every mount is stepped, not just the current room's: a turn started in one
   room and walked out of should still be finished when the player comes back,
   and the cost is a handful of mounts either way. */
void valve_handles_update(void) {
    int i;
    for (i = 0; i < valve_mount_count; i++) {
        ValveMount *m = &valve_mounts[i];
        if (!m->active || !m->present || m->turn_timer <= 0) continue;

        m->turn_timer--;

        int32_t done = VALVE_TURN_FRAMES - m->turn_timer;   /* 1..FRAMES */
        m->spin = (int32_t)(((int64_t)done * 4096 * VALVE_TURN_REVS)
                            / VALVE_TURN_FRAMES) & 4095;
        if (m->turn_timer == 0) m->spin = 0;                /* land on true */
    }
}

void valve_handles_draw(RenderContext *ctx) {
    if (!valve_smd) return;

    MATRIX view;
    camera_build_view(&view);

    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;

    uint16_t pipe_tpage, pipe_clut;
    valve_pipe_slot(current_area, &pipe_tpage, &pipe_clut);

    int d;
    for (d = 0; d < valve_mount_count; d++) {
        ValveMount *m = &valve_mounts[d];
        if (!m->active || !m->present) continue;
        if (m->area != current_area) continue;

        int32_t ddx = m->x - cam_x, ddz = m->z - cam_z;
        int32_t ddist = (ddx < 0 ? -ddx : ddx) + (ddz < 0 ? -ddz : ddz);
        if (ddist > g_fog_far) continue;

        int32_t fog_factor = render_fog_scale(ddist);

        /* TWO ROTATIONS, COMPOSED EXPLICITLY, and the order is the whole point.
           The SPIN is about the model's own Y — the stem, the axis a valve
           actually turns on — and the MOUNT then takes that already-spun wheel
           and stands it against the pipe. Get it the other way round and the
           wheel wobbles about a world axis instead of turning.

           PSn00bSDK's RotMatrix builds Rx * Ry * Rz (see its header), so one
           call with {rot_x, spin, 0} would in fact produce the same matrix —
           the Y is applied first. This is written out anyway: the mount is a
           fixed pose and the spin is an animation, they are not two components
           of one Euler angle, and a mount that ever needs a rot_z would silently
           start composing in the wrong order. MulMatrix0(a, b, c) gives
           c = a * b, so mount * spin applies the spin first. */
        MATRIX spin_m, mount_m, world_m, combined;
        SVECTOR spin_r  = {0, (short)m->spin, 0, 0};
        SVECTOR mount_r = {(short)m->rot_x, (short)m->rot_y, 0, 0};
        RotMatrix(&spin_r,  &spin_m);
        RotMatrix(&mount_r, &mount_m);
        MulMatrix0(&mount_m, &spin_m, &world_m);

        VECTOR pos = {m->x, m->y, m->z};
        TransMatrix(&world_m, &pos);
        CompMatrixLV(&view, &world_m, &combined);
        gte_SetRotMatrix(&combined);
        gte_SetTransMatrix(&combined);

        /* The fat door's manual loop again, with two changes: the model carries
           FT3s as well as FT4s (it is a ring, so its caps triangulate), and every
           textured primitive takes THIS ROOM'S pipe slot rather than the one
           smxlink baked in. Untextured primitives are skipped — see the note on
           the red marker poly in valve_handle.h. */
        uint8_t *p = (uint8_t *)valve_smd->p_prims;
        int i;
        for (i = 0; i < valve_smd->n_prims; i++) {
            SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
            uint8_t stride = pt->len;
            int is_quad = (pt->type >= 2);

            if (!pt->texture) { p += stride; continue; }

            uint16_t *vi = (uint16_t *)(p + 4);
            SVECTOR *v0 = &valve_smd->p_verts[vi[0]];
            SVECTOR *v1 = &valve_smd->p_verts[vi[1]];
            SVECTOR *v2 = &valve_smd->p_verts[vi[2]];

            DVECTOR sv[4];
            int32_t sz[4];
            int32_t otz, nclip;

            gte_ldv3(v0, v1, v2);
            gte_rtpt();
            gte_stsxy3c(sv);

            if (sv[0].vx <= -1023 || sv[0].vx >= 1023 || sv[0].vy <= -1023 || sv[0].vy >= 1023 ||
                sv[1].vx <= -1023 || sv[1].vx >= 1023 || sv[1].vy <= -1023 || sv[1].vy >= 1023 ||
                sv[2].vx <= -1023 || sv[2].vx >= 1023 || sv[2].vy <= -1023 || sv[2].vy >= 1023) {
                p += stride; continue;
            }

            if (!pt->nocull) {
                gte_nclip();
                gte_stopz(&nclip);
                if (nclip <= 0) { p += stride; continue; }
            }

            gte_stsz4c(sz);
            if (sz[1] == 0 || sz[2] == 0 || sz[3] == 0) { p += stride; continue; }

            if (is_quad) {
                SVECTOR *v3 = &valve_smd->p_verts[vi[3]];
                gte_ldv0(v3);
                gte_rtps();
                gte_stsxy(&sv[3]);
                gte_stsz(&sz[3]);
                if (sv[3].vx <= -1023 || sv[3].vx >= 1023 ||
                    sv[3].vy <= -1023 || sv[3].vy >= 1023) { p += stride; continue; }
                if (sz[3] == 0) { p += stride; continue; }
                gte_avsz4();
            } else {
                gte_avsz3();
            }

            gte_stotz(&otz);
            if (otz <= 0) { p += stride; continue; }
            /* A SMALLER OT BIAS THAN THE FAT DOOR'S 40. The wheel stands 20
               units off a wall it is meant to look bolted to, and at that
               separation a +40 push puts it BEHIND the pipe from some angles.
               +8 is enough to keep it off the room mesh's own primitives without
               swapping the two. */
            otz += 8;
            if (otz >= OT_LENGTH - 1) otz = OT_LENGTH - 2;

            uint8_t *col = p + 16;
            uint8_t r = (uint8_t)(((int32_t)col[0] * fog_factor + SKY_FOG_R * (256 - fog_factor)) >> 8);
            uint8_t g = (uint8_t)(((int32_t)col[1] * fog_factor + SKY_FOG_G * (256 - fog_factor)) >> 8);
            uint8_t b = (uint8_t)(((int32_t)col[2] * fog_factor + SKY_FOG_B * (256 - fog_factor)) >> 8);

            uint8_t *uv = p + 20;   /* UVs sit at +20 for FT3 and FT4 alike */

            if (is_quad) {
                if (ctx->next_packet + sizeof(POLY_FT4) > buf_end) { p += stride; continue; }
                POLY_FT4 *poly = (POLY_FT4 *)ctx->next_packet;
                setPolyFT4(poly);
                setRGB0(poly, r, g, b);
                poly->tpage = pipe_tpage;
                poly->clut  = pipe_clut;
                poly->u0=uv[0]; poly->v0=uv[1];
                poly->u1=uv[2]; poly->v1=uv[3];
                poly->u2=uv[4]; poly->v2=uv[5];
                poly->u3=uv[6]; poly->v3=uv[7];
                poly->x0=sv[0].vx; poly->y0=sv[0].vy;
                poly->x1=sv[1].vx; poly->y1=sv[1].vy;
                poly->x2=sv[2].vx; poly->y2=sv[2].vy;
                poly->x3=sv[3].vx; poly->y3=sv[3].vy;
                addPrim(&ctx->buffers[ctx->active_buffer].ot[otz], poly);
                ctx->next_packet += sizeof(POLY_FT4);
            } else {
                if (ctx->next_packet + sizeof(POLY_FT3) > buf_end) { p += stride; continue; }
                POLY_FT3 *poly = (POLY_FT3 *)ctx->next_packet;
                setPolyFT3(poly);
                setRGB0(poly, r, g, b);
                poly->tpage = pipe_tpage;
                poly->clut  = pipe_clut;
                poly->u0=uv[0]; poly->v0=uv[1];
                poly->u1=uv[2]; poly->v1=uv[3];
                poly->u2=uv[4]; poly->v2=uv[5];
                poly->x0=sv[0].vx; poly->y0=sv[0].vy;
                poly->x1=sv[1].vx; poly->y1=sv[1].vy;
                poly->x2=sv[2].vx; poly->y2=sv[2].vy;
                addPrim(&ctx->buffers[ctx->active_buffer].ot[otz], poly);
                ctx->next_packet += sizeof(POLY_FT3);
            }

            p += stride;
        }
    }

    /* Restore the plain view matrix so later world-space draws project right. */
    gte_SetRotMatrix(&view);
    gte_SetTransMatrix(&view);
}
