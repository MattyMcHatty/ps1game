#include <stdint.h>
#include <stddef.h>
#include "room_data.h"
#include "room_arena.h"

/* See room_data.h. The block layout is documented once, in tools/pack_rooms.py;
   the offsets below are that table. */

#define RD_MAGIC    0x54414452u   /* 'RDAT' little-endian */
#define RD_FOOTER   0x54464452u   /* 'RDFT' */
#define RD_HEADER   36            /* bytes before the tex map */

const uint8_t *room_tex_map     = NULL;
const uint8_t *room_nocull_bits = NULL;

static const uint8_t *rd_block = NULL;   /* bound block, or NULL */
static const int16_t *rd_walls = NULL;
static int            rd_wall_count = 0;

static uint16_t rd_u16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void room_data_unbind(void) {
    room_tex_map     = NULL;
    room_nocull_bits = NULL;
    rd_block         = NULL;
    rd_walls         = NULL;
    rd_wall_count    = 0;
}

int room_data_bind(int n_prims) {
    /* room_arena_load() returns the arena base, and that is the only buffer a
       room's file is ever in. */
    const uint8_t *file;
    int size = room_arena_file_size();
    uint32_t off;
    int prims, walls, map_end;

    room_data_unbind();
    if (size < 8) return 0;
    file = (const uint8_t *)room_arena_base();

    if (rd_u32(file + size - 4) != RD_FOOTER) return 0;
    off = rd_u32(file + size - 8);
    if ((off & 3) || off + RD_HEADER > (uint32_t)size - 8) return 0;
    if (rd_u32(file + off) != RD_MAGIC) return 0;
    if (rd_u16(file + off + 4) != ROOM_DATA_VERSION) return 0;

    prims = rd_u16(file + off + 6);
    walls = rd_u16(file + off + 8);
    if (prims != n_prims || walls > MAX_WALLS_PER_ROOM) return 0;

    map_end = RD_HEADER + prims + (prims + 7) / 8;
    map_end = (map_end + 3) & ~3;
    if (off + map_end + walls * 16 != (uint32_t)size - 8) return 0;

    rd_block         = file + off;
    room_tex_map     = rd_block + RD_HEADER;
    room_nocull_bits = rd_block + RD_HEADER + prims;
    rd_walls         = (const int16_t *)(rd_block + map_end);   /* 4-aligned */
    rd_wall_count    = walls;
    return 1;
}

void room_data_collision(CollisionRoom *r) {
    int i;
    if (!rd_block) {
        r->wall_count      = 0;
        r->min_x = r->max_x = r->min_z = r->max_z = 0;
        r->multi_level     = 0;
        r->shoot_over_mask = 0;
        return;
    }
    r->wall_count  = rd_wall_count;
    r->multi_level = rd_u16(rd_block + 10);
    r->min_x = (int32_t)rd_u32(rd_block + 12);
    r->max_x = (int32_t)rd_u32(rd_block + 16);
    r->min_z = (int32_t)rd_u32(rd_block + 20);
    r->max_z = (int32_t)rd_u32(rd_block + 24);
    r->shoot_over_mask = (uint64_t)rd_u32(rd_block + 28) |
                         ((uint64_t)rd_u32(rd_block + 32) << 32);
    for (i = 0; i < rd_wall_count; i++) {
        const int16_t *w = rd_walls + i * 8;
        r->walls[i].x1    = w[0];  r->walls[i].z1    = w[1];
        r->walls[i].x2    = w[2];  r->walls[i].z2    = w[3];
        r->walls[i].nx    = w[4];  r->walls[i].nz    = w[5];
        r->walls[i].y_min = w[6];  r->walls[i].y_max = w[7];
    }
}
