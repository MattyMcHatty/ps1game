#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <psxcd.h>
#include <psxpad.h>
#include <inline_c.h>
#include <smd/smd.h>
#include "render.h"
#include "camera.h"
#include "collision.h"      /* GROUND_FLOOR_Y */
#include "texmgr.h"
#include "title.h"          /* current_area gate: the prop only lives in the 2F hall */
#include "trick_drawers.h"
#include "player.h"         /* player_items, show_pickup_msg_raw */
#include "sound.h"          /* SFX_UNLOCK */
#include "door.h"           /* door_draw_string_billboard */
#include "btn_glyph.h"      /* BTN_*, btn_prompt_draw */

extern volatile uint8_t pad_buff[2][34];
extern volatile size_t  pad_buff_len[2];

#define MAX_TRICK_DRAWERS   4
#define TD_PUSH_MARGIN     30   /* extra gap between player and prop edge */

/* Footprint half-extents + solid height, from the SMX bbox: 50 wide (x +/-25),
   250 deep (z +/-125), ~313 tall (resized-down model, July 2026). */
#define TD_HALF_W   25
#define TD_HALF_D  125
#define TD_SOLID_H 313

typedef struct {
    int32_t x, y, z, rot_y;   /* y = standing reference; world = y+GROUND_FLOOR_Y */
    int32_t active;
} DrawerProp;

static DrawerProp props[MAX_TRICK_DRAWERS];
static int        prop_count = 0;

static SMD  *drawer_smd = NULL;
static void *drawer_buf = NULL;
static int   drawer_tex = -1;   /* texmgr id of the closed-drawer texture */

static int opn_tex = -1;   /* texmgr id of the open-drawer texture (time-shared) */

/* ---- Drawer puzzle -------------------------------------------------------
   The front face is a 4-col x 5-row grid of square drawer polys. Grid coords are
   (screen) row 0 = TOP, col 0 = LEFT; drawer number = row*4 + col + 1 (1..20).
   In the model, local -Y is up, so the TOP row is the most-negative local y. */
static const int16_t ROW_Y[5] = { -281, -219, -156, -94, -31 };  /* top -> bottom */
static const int16_t COL_Z[4] = {  -94,  -31,   31,  94 };       /* left -> right  */
#define CELL_HALF   31
#define FACE_X      26     /* just in front of the front plane (x=25) */

/* SMD prim index -> drawer number (1..20); 0 = not a drawer face. Derived from
   the mesh's per-face y/z centres (see gen analysis). */
static const uint8_t PRIM_DRAWER[40] = {
    [3]=1,  [7]=2,  [6]=3,  [5]=4,     /* top row  (local y=-281) */
    [33]=5, [17]=6, [21]=7, [25]=8,    /* y=-219 */
    [32]=9, [16]=10,[20]=11,[24]=12,   /* y=-156 */
    [31]=13,[15]=14,[19]=15,[23]=16,   /* y=-94  */
    [30]=17,[14]=18,[18]=19,[22]=20,   /* bottom row (local y=-31) */
};

/* The unlock sequence (drawer numbers). */
static const uint8_t SOLUTION[6] = { 6, 15, 2, 13, 9, 20 };

/* Face-on camera target: centred on the drawer front (world), looking -Z. The
   prop's model origin sits at world y=0 (draw uses y+GROUND_FLOOR_Y=0), so the
   face (local y -312..0) is centred at world y=-156. */
#define TD_CAM_X    (-3299)
#define TD_CAM_Y     (-156)   /* face vertical centre                       */
#define TD_CAM_Z     (-895)   /* ~440 in front of the face (z=-1334)        */
#define TD_CAM_ROT    2048     /* face -Z, straight at the drawers           */

/* Proximity interact prompt/trigger, centred on the prop. */
#define TD_INTERACT_X   (-3299)
#define TD_INTERACT_Z   (-1360)
#define TD_INTERACT_RADIUS       600   /* Circle-press interact range */
#define TD_INTERACT_TEXT_RADIUS 1000   /* prompt visible (fades out) within this */
#define TD_INTERACT_FADE_NEAR    800   /* fully opaque within this distance */

static int      puzzle_active = 0;
static int      puzzle_ready  = 0;    /* camera settled, input live */
static uint32_t puzzle_open   = 0;    /* bit (d-1) set = drawer d shown open */
static int      puzzle_prog   = 0;    /* correct selections so far (0..6) */
static int      ret_row = 0, ret_col = 0;

static int32_t  save_cx, save_cy, save_cz, save_crot, save_cvy;   /* pre-puzzle camera */
static int32_t  cam_anim_t = 0;
#define CAM_ANIM_FRAMES 24
static int32_t  src_x, src_y, src_z, src_rot, rot_delta;

static uint16_t pz_btn_prev  = 0xFFFF; /* puzzle input edge-detect (btn = ~pad) */
static int      interact_prev = 1;     /* interact-Circle edge-detect */

/* Reticule screen rect, refreshed by the projector in trick_drawers_draw. */
static int ret_sx, ret_sy, ret_sw, ret_sh, ret_valid = 0;

static void *read_file(const char *name) {
    CdlFILE file;
    if (!CdSearchFile(&file, (char *)name)) return NULL;
    int sectors = (file.size + 2047) / 2048;
    void *buf = malloc(sectors * 2048);
    if (!buf) return NULL;
    CdControl(CdlSetloc, &file.pos, NULL);
    CdRead(sectors, (uint32_t *)buf, CdlModeSpeed);
    CdReadSync(0, NULL);
    return buf;
}

/* Startup: load the model SMD and register the closed-drawer texture (RAM-
   resident for a pure-LoadImage upload on room entry). */
void trick_drawers_load_assets(void) {
    drawer_buf = read_file("\\TEX\\TRKDRWR.SMD;1");
    if (drawer_buf) drawer_smd = smdInitData(drawer_buf);

    drawer_tex = texmgr_register("\\TEX\\CLSDDRWR.TIM;1");
    opn_tex    = texmgr_register("\\TEX\\OPNDRWR.TIM;1");
}

/* Upload the drawer texture into its VRAM slot (cncrte's / kchn_tile's, which
   the 2F hall does not otherwise use) from the resident RAM copy. Pure
   LoadImage — safe during the room transition (caller DrawSyncs first). */
void trick_drawers_upload_texture(void) {
    if (drawer_tex >= 0) texmgr_upload(drawer_tex);
    if (opn_tex    >= 0) texmgr_upload(opn_tex);
}

static void add_prop(int32_t x, int32_t y, int32_t z, int32_t rot_y) {
    if (prop_count >= MAX_TRICK_DRAWERS) return;
    DrawerProp *p = &props[prop_count++];
    p->x = x; p->y = y; p->z = z;
    p->rot_y = rot_y;
    p->active = 1;
}

/* Place the hall's drawers. y = -149 seats a base-origin model on the floor
   (world y=0). One chest centred against the west room's south wall (z=-1398,
   spanning x[-3797,-2801], middle x=-3299), rotated 90deg (rot 3072) so the
   drawer front (+X in the model) faces north (+Z) into the room. */
void trick_drawers_place(void) {
    prop_count = 0;
    add_prop(-3299, -149, -1360, 3072);
    /* Reset the puzzle each time the hall is (re)entered. */
    puzzle_active = 0; puzzle_ready = 0;
    puzzle_open = 0; puzzle_prog = 0;
    ret_row = 0; ret_col = 0;
    interact_prev = 1;
}

/* Axis-aligned bound of the rotated footprint (same maths as the dresser). */
static void rotated_half_extents(int32_t rot_y, int32_t *hw, int32_t *hd) {
    int32_t c = icos(rot_y), s = isin(rot_y);
    if (c < 0) c = -c;
    if (s < 0) s = -s;
    *hw = (TD_HALF_W * c + TD_HALF_D * s) >> 12;
    *hd = (TD_HALF_W * s + TD_HALF_D * c) >> 12;
}

/* Player push-out (dresser-style Minkowski AABB). Gated to the 2F hall so the
   shared reception collision routine can call this unconditionally. */
void trick_drawers_collide(int32_t *px, int32_t py, int32_t *pz, int32_t radius) {
    (void)py;   /* single flat floor — no vertical gating needed */
    if (current_area != STATE_2F_HALL) return;
    int i;
    for (i = 0; i < prop_count; i++) {
        DrawerProp *p = &props[i];
        if (!p->active) continue;

        int32_t hw, hd;
        rotated_half_extents(p->rot_y, &hw, &hd);

        int32_t min_x = p->x - hw - radius - TD_PUSH_MARGIN;
        int32_t max_x = p->x + hw + radius + TD_PUSH_MARGIN;
        int32_t min_z = p->z - hd - radius - TD_PUSH_MARGIN;
        int32_t max_z = p->z + hd + radius + TD_PUSH_MARGIN;

        if (*px <= min_x || *px >= max_x) continue;
        if (*pz <= min_z || *pz >= max_z) continue;

        /* Push out along the axis with the smallest penetration. */
        int32_t push_l = *px - min_x;
        int32_t push_r = max_x - *px;
        int32_t push_f = *pz - min_z;
        int32_t push_b = max_z - *pz;

        int32_t min_push = push_l, px_delta = -push_l, pz_delta = 0;
        if (push_r < min_push) { min_push = push_r; px_delta =  push_r; pz_delta = 0; }
        if (push_f < min_push) { min_push = push_f; px_delta = 0; pz_delta = -push_f; }
        if (push_b < min_push) {                    px_delta = 0; pz_delta =  push_b; }

        *px += px_delta;
        *pz += pz_delta;
    }
}

/* ---- Puzzle state machine ------------------------------------------------ */
int trick_drawers_puzzle_active(void) { return puzzle_active; }

static void start_puzzle(void) {
    save_cx = cam_x; save_cy = cam_y; save_cz = cam_z;
    save_crot = cam_rot; save_cvy = cam_vy;

    src_x = cam_x; src_y = cam_y; src_z = cam_z; src_rot = cam_rot;
    /* Shortest signed turn from the current heading to face-on. */
    int32_t d = ((TD_CAM_ROT - src_rot) % 4096 + 4096) % 4096;
    if (d > 2048) d -= 4096;
    rot_delta = d;

    cam_anim_t    = 0;
    puzzle_active = 1;
    puzzle_ready  = 0;
    ret_row = 0; ret_col = 0;
    ret_valid = 0;
}

static void exit_puzzle(int reset) {
    puzzle_active = 0; puzzle_ready = 0; ret_valid = 0;
    cam_x = save_cx; cam_y = save_cy; cam_z = save_cz;
    cam_rot = save_crot; cam_vy = save_cvy;
    if (reset) { puzzle_open = 0; puzzle_prog = 0; }
    interact_prev = 1;   /* swallow the held Circle so we don't re-open instantly */
}

static void select_drawer(void) {
    int d = ret_row * 4 + ret_col + 1;
    if (d == SOLUTION[puzzle_prog]) {
        puzzle_open |= (1u << (d - 1));
        puzzle_prog++;
        if (puzzle_prog >= 6) {
            /* Final drawer: award the Wax Cube and kick the player out. */
            player_items |= (1 << ITEM_WAX_CUBE);
            sound_play(SFX_PICKUP);
            show_pickup_msg_raw("Received the Wax Cube");
            exit_puzzle(1);
        } else {
            sound_play(SFX_UNLOCK);
            show_pickup_msg_raw("You hear another drawer unlock");
        }
    } else if (puzzle_prog == 0) {
        /* Nothing open yet: any wrong drawer is simply locked (stay in puzzle). */
        show_pickup_msg_raw("The drawer is locked");
    } else {
        /* Past the first: a wrong drawer slams them all shut and ejects. */
        sound_play(SFX_SLAM);
        show_pickup_msg_raw("All the drawers slam shut!");
        exit_puzzle(1);
    }
}

/* Once the Wax Cube is won the puzzle is finished for good. */
static int solved(void) { return (player_items & (1 << ITEM_WAX_CUBE)) != 0; }

void trick_drawers_update(void) {
    uint16_t btn = 0;
    if (pad_buff_len[0]) { PadResponse *pad = (PadResponse *)pad_buff[0]; btn = ~pad->btn; }

    if (!puzzle_active) {
        if (solved()) return;   /* already solved: no more interacting */
        int held = interact_tapped();
        int just = held && !interact_prev;
        interact_prev = held;
        int32_t dx = cam_x - TD_INTERACT_X, dz = cam_z - TD_INTERACT_Z;
        int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
        if (just && xz < TD_INTERACT_RADIUS) start_puzzle();
        return;
    }

    /* Camera glides to the face-on view; input opens once it settles. */
    if (!puzzle_ready) {
        cam_anim_t++;
        int32_t t = cam_anim_t * 256 / CAM_ANIM_FRAMES; if (t > 256) t = 256;
        int32_t inv = 256 - t;
        int32_t e = 256 - (inv * inv / 256);            /* ease-out 0..256 */
        cam_x   = src_x   + ((TD_CAM_X   - src_x)   * e) / 256;
        cam_y   = src_y   + ((TD_CAM_Y   - src_y)   * e) / 256;
        cam_z   = src_z   + ((TD_CAM_Z   - src_z)   * e) / 256;
        cam_rot = src_rot + (rot_delta * e) / 256;
        cam_vy  = 0;
        if (cam_anim_t >= CAM_ANIM_FRAMES) {
            cam_x = TD_CAM_X; cam_y = TD_CAM_Y; cam_z = TD_CAM_Z;
            cam_rot = TD_CAM_ROT; cam_vy = 0;
            puzzle_ready = 1;
            pz_btn_prev  = btn;   /* arm: don't treat the held Circle as a select */
        }
        return;
    }

    uint16_t pressed = btn & ~pz_btn_prev;
    pz_btn_prev = btn;

    if ((pressed & PAD_UP)    && ret_row > 0) ret_row--;
    if ((pressed & PAD_DOWN)  && ret_row < 4) ret_row++;
    if ((pressed & PAD_LEFT)  && ret_col > 0) ret_col--;
    if ((pressed & PAD_RIGHT) && ret_col < 3) ret_col++;

    if (pressed & PAD_CROSS)  { exit_puzzle(0); return; }   /* leave, keep progress */
    if (pressed & PAD_CIRCLE) { select_drawer(); }
}

/* Filled screen rect (2D overlay) at OT index ot_idx. */
static void puzzle_rect(RenderContext *ctx, int x, int y, int w, int h,
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

static void puzzle_outline(RenderContext *ctx, int x, int y, int w, int h,
                           uint8_t r, uint8_t g, uint8_t b, int ot_idx) {
    puzzle_rect(ctx, x,       y,       w, 2, r, g, b, ot_idx);
    puzzle_rect(ctx, x,       y+h-2,   w, 2, r, g, b, ot_idx);
    puzzle_rect(ctx, x,       y,       2, h, r, g, b, ot_idx);
    puzzle_rect(ctx, x+w-2,   y,       2, h, r, g, b, ot_idx);
}

/* Render all placed drawers with the hall's fog and texture window. Restores the
   caller's view matrix before returning. */
void trick_drawers_draw(RenderContext *ctx) {
    if (prop_count == 0 || !drawer_smd) return;

    MATRIX view;
    camera_build_view(&view);

    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    uint16_t tp = texmgr_tpage(drawer_tex);
    uint16_t cl = texmgr_clut(drawer_tex);
    uint16_t opn_tp = texmgr_tpage(opn_tex);
    uint16_t opn_cl = texmgr_clut(opn_tex);

    int i;
    for (i = 0; i < prop_count; i++) {
        DrawerProp *pr = &props[i];
        if (!pr->active) continue;

        int32_t dcx = pr->x - cam_x, dcz = pr->z - cam_z;
        if ((dcx < 0 ? -dcx : dcx) + (dcz < 0 ? -dcz : dcz) > 1500) continue;

        MATRIX pm, combined;
        SVECTOR prot = {0, pr->rot_y, 0, 0};
        RotMatrix(&prot, &pm);
        VECTOR pos = {pr->x, pr->y + GROUND_FLOOR_Y, pr->z};
        TransMatrix(&pm, &pos);
        CompMatrixLV(&view, &pm, &combined);

        gte_SetRotMatrix(&combined);
        gte_SetTransMatrix(&combined);

        uint8_t *p = (uint8_t *)drawer_smd->p_prims;
        int pi;
        for (pi = 0; pi < drawer_smd->n_prims; pi++) {
            SMD_PRI_TYPE *pt     = (SMD_PRI_TYPE *)p;
            uint8_t       stride = pt->len;
            int           is_quad = (pt->type >= 2);

            uint16_t *vi = (uint16_t *)(p + 4);
            SVECTOR *v0 = &drawer_smd->p_verts[vi[0]];
            SVECTOR *v1 = &drawer_smd->p_verts[vi[1]];
            SVECTOR *v2 = &drawer_smd->p_verts[vi[2]];

            DVECTOR sv[4];
            int32_t sz[4], otz, nclip;

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

            SVECTOR *v3    = 0;
            int32_t  v2_sz = sz[3];   /* v2's SZ, before the quad path reuses sz[3] */
            if (is_quad) {
                v3 = &drawer_smd->p_verts[vi[3]];
                gte_ldv0(v3);
                gte_rtps();
                gte_stsxy(&sv[3]);
                gte_stsz(&sz[3]);
                if (sv[3].vx <= -1023 || sv[3].vx >= 1023 || sv[3].vy <= -1023 || sv[3].vy >= 1023) { p += stride; continue; }
                if (sz[3] == 0) { p += stride; continue; }
                gte_avsz4();
            } else {
                gte_avsz3();
            }

            gte_stotz(&otz);
            /* Horizontal polys sort by their farthest corner (see render.h). */
            if (poly_is_flat_y(v0, v1, v2, v3))
                otz = is_quad ? otz_far4(sz[1], sz[2], v2_sz, sz[3])
                              : otz_far3(sz[1], sz[2], sz[3]);
            if (otz <= 0) { p += stride; continue; }
            otz += 40;
            if (otz >= OT_LENGTH - 1) otz = OT_LENGTH - 2;

            int32_t fog_start = 350, fog_end = 1500;   /* matches the room */
            int32_t dist = (dcx < 0 ? -dcx : dcx) + (dcz < 0 ? -dcz : dcz);
            int32_t fog = dist < fog_start ? fog_start : (dist > fog_end ? fog_end : dist);
            int32_t fog_factor = ((fog_end - fog) << 8) / (fog_end - fog_start);

            uint8_t *col = p + 16;
            uint8_t r = (uint8_t)(((int32_t)col[0] * fog_factor + 20 * (256 - fog_factor)) >> 8);
            uint8_t g = (uint8_t)(((int32_t)col[1] * fog_factor + 15 * (256 - fog_factor)) >> 8);
            uint8_t b = (uint8_t)(((int32_t)col[2] * fog_factor + 10 * (256 - fog_factor)) >> 8);

            /* Opened drawer faces swap to the open texture (same UVs; opn_drwr is
               128x128 at Voff 0, so the hall's 128 window tiles it identically). */
            uint16_t ptp = tp, pcl = cl;
            {
                int dn = (pi < 40) ? PRIM_DRAWER[pi] : 0;
                if (dn && (puzzle_open & (1u << (dn - 1)))) { ptp = opn_tp; pcl = opn_cl; }
            }

            if (is_quad) {
                if (ctx->next_packet + sizeof(POLY_FT4) > buf_end) { p += stride; continue; }
                uint8_t *uv = p + 20;
                POLY_FT4 *poly = (POLY_FT4 *)ctx->next_packet;
                setPolyFT4(poly);
                setRGB0(poly, r, g, b);
                poly->tpage = ptp;
                poly->clut  = pcl;
                poly->u0=uv[0]; poly->v0=uv[1];
                poly->u1=uv[2]; poly->v1=uv[3];
                poly->u2=uv[4]; poly->v2=uv[5];
                poly->u3=uv[6]; poly->v3=uv[7];
                poly->x0 = sv[0].vx; poly->y0 = sv[0].vy;
                poly->x1 = sv[1].vx; poly->y1 = sv[1].vy;
                poly->x2 = sv[2].vx; poly->y2 = sv[2].vy;
                poly->x3 = sv[3].vx; poly->y3 = sv[3].vy;
                addPrim(&ctx->buffers[ctx->active_buffer].ot[otz], poly);
                ctx->next_packet += sizeof(POLY_FT4);
            } else {
                if (ctx->next_packet + sizeof(POLY_FT3) > buf_end) { p += stride; continue; }
                uint8_t *uv = p + 20;
                POLY_FT3 *poly = (POLY_FT3 *)ctx->next_packet;
                setPolyFT3(poly);
                setRGB0(poly, r, g, b);
                poly->tpage = ptp;
                poly->clut  = pcl;
                poly->u0=uv[0]; poly->v0=uv[1];
                poly->u1=uv[2]; poly->v1=uv[3];
                poly->u2=uv[4]; poly->v2=uv[5];
                poly->x0 = sv[0].vx; poly->y0 = sv[0].vy;
                poly->x1 = sv[1].vx; poly->y1 = sv[1].vy;
                poly->x2 = sv[2].vx; poly->y2 = sv[2].vy;
                addPrim(&ctx->buffers[ctx->active_buffer].ot[otz], poly);
                ctx->next_packet += sizeof(POLY_FT3);
            }

            p += stride;
        }

        /* Project the current reticule cell to screen while this prop's matrix is
           still loaded (the drawers are prop 0). */
        if (puzzle_active && puzzle_ready && i == 0) {
            int minx = 99999, miny = 99999, maxx = -99999, maxy = -99999;
            int dyv, dzv;
            for (dyv = -1; dyv <= 1; dyv += 2)
                for (dzv = -1; dzv <= 1; dzv += 2) {
                    SVECTOR c;
                    c.vx = FACE_X;
                    c.vy = (int16_t)(ROW_Y[ret_row] + dyv * CELL_HALF);
                    c.vz = (int16_t)(COL_Z[ret_col] + dzv * CELL_HALF);
                    c.pad = 0;
                    DVECTOR s;
                    gte_ldv0(&c); gte_rtps(); gte_stsxy(&s);
                    if (s.vx < minx) minx = s.vx;
                    if (s.vx > maxx) maxx = s.vx;
                    if (s.vy < miny) miny = s.vy;
                    if (s.vy > maxy) maxy = s.vy;
                }
            ret_sx = minx; ret_sy = miny; ret_sw = maxx - minx; ret_sh = maxy - miny;
            ret_valid = 1;
        }
    }

    /* Restore the camera view matrix for whatever the caller draws next. */
    gte_SetRotMatrix(&view);
    gte_SetTransMatrix(&view);

    /* Puzzle overlays (2D) / proximity prompt (billboard). */
    if (puzzle_active) {
        if (puzzle_ready && ret_valid) {
            puzzle_outline(ctx, ret_sx - 4, ret_sy - 4, ret_sw + 8, ret_sh + 8,
                           80, 80, 200, 3);
            puzzle_outline(ctx, ret_sx - 2, ret_sy - 2, ret_sw + 4, ret_sh + 4,
                           180, 180, 255, 2);
            btn_prompt_draw(ctx, 8, 206,
                            BTN_CIRCLE " - Select  " BTN_CROSS " - Exit", 1);
        }
    } else if (!solved()) {
        int32_t dx = cam_x - TD_INTERACT_X, dz = cam_z - TD_INTERACT_Z;
        int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
        if (xz < TD_INTERACT_TEXT_RADIUS) {
            /* Fade in as the player approaches (fully opaque within FADE_NEAR),
               same curve as the door signs. Two stacked lines. */
            int fade = 256;
            if (xz > TD_INTERACT_FADE_NEAR) {
                int range = TD_INTERACT_TEXT_RADIUS - TD_INTERACT_FADE_NEAR;
                int prog  = xz - TD_INTERACT_FADE_NEAR;
                if (prog > range) prog = range;
                fade = 256 - ((prog * 256) / range);
            }
            door_draw_string_billboard(ctx, "Press " BTN_CIRCLE,
                                       -3299, -206, -1290,
                                       50, 255, 50, fade, DOOR_PIXEL_SIZE);
            door_draw_string_billboard(ctx, "to interact",
                                       -3299, -170, -1290,
                                       50, 255, 50, fade, DOOR_PIXEL_SIZE);
        }
    }
}
