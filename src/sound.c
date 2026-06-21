#include <stdint.h>
#include <stdlib.h>
#include <psxspu.h>
#include <psxcd.h>
#include "sound.h"

/* SPU RAM layout — first 0x1000 reserved, SpuInit uploads dummy at 0x1000 */
#define ALLOC_START 0x1010
#define FIRST_VOICE 1

typedef struct {
    int spu_addr;
    int sample_rate;
    int loaded;
} SfxSlot;

static SfxSlot sfx_slots[SFX_COUNT];
static int     next_spu_addr = ALLOC_START;

/* VAGs live in the \SND subdirectory (see disc.xml): keeping them out of the
   root directory stops it spilling into a second sector, which the PS1 boot ROM
   can't follow to find SYSTEM.CNF (the disc would freeze at the BIOS logo). */
static const char *sfx_files[SFX_COUNT] = {
    "\\SND\\SWING.VAG;1",
    "\\SND\\HURT.VAG;1",
    "\\SND\\PICKUP.VAG;1",
    "\\SND\\SMASH.VAG;1",
    "\\SND\\DOGBARK.VAG;1",
    "\\SND\\DOGHURT.VAG;1",
    "\\SND\\DOGDIE.VAG;1",
    "\\SND\\UNLOCK.VAG;1",
    "\\SND\\DROPEN.VAG;1",
    "\\SND\\ZOMBIE.VAG;1",
    "\\SND\\ZOMBIEDIE.VAG;1",
};

/* Which SPU voice a sound plays on. Short one-shot effects share a small pool
   (FIRST_VOICE + id%8). The zombie groan is a long, continuous ambience that
   must NOT be cut by a one-shot (e.g. the player's hurt sound shares its slot
   under id%8), so it gets a dedicated voice clear of that pool. */
static int sfx_channel(SfxID id) {
    if (id == SFX_ZOMBIE) return 16;
    return FIRST_VOICE + (id % 8);
}

static void load_vag(SfxID id) {
    CdlFILE file;
    if (!CdSearchFile(&file, sfx_files[id])) return;

    int sectors = (file.size + 2047) / 2048;
    void *buf   = malloc(sectors * 2048);
    if (!buf) return;

    CdControl(CdlSetloc, &file.pos, NULL);
    CdRead(sectors, (uint32_t *)buf, CdlModeSpeed);
    CdReadSync(0, NULL);

    uint8_t *raw = (uint8_t *)buf;

    /* VAG header: size at offset 12 (big-endian), sample rate at offset 16 */
    uint32_t audio_size = ((uint32_t)raw[12] << 24) |
                          ((uint32_t)raw[13] << 16) |
                          ((uint32_t)raw[14] <<  8) |
                          ((uint32_t)raw[15]);
    uint32_t rate       = ((uint32_t)raw[16] << 24) |
                          ((uint32_t)raw[17] << 16) |
                          ((uint32_t)raw[18] <<  8) |
                          ((uint32_t)raw[19]);

    /* Audio data starts after 48-byte header */
    uint8_t *audio    = raw + 48;
    /* Round up to 64-byte blocks — SPU DMA transfers in 64-byte units */
    int      dma_size = (audio_size + 63) & ~63;

    /* Convert to KSEG1 (uncached) so SPU DMA reads physical RAM directly,
       avoiding cache coherency issues with the preceding CdRead DMA. */
    const uint32_t *audio_uncached =
        (const uint32_t *)((uint32_t)audio | 0xA0000000);

    SpuSetTransferMode(SPU_TRANSFER_BY_DMA);
    SpuSetTransferStartAddr(next_spu_addr);
    SpuWrite(audio_uncached, dma_size);
    SpuIsTransferCompleted(SPU_TRANSFER_WAIT);

    sfx_slots[id].spu_addr    = next_spu_addr;
    sfx_slots[id].sample_rate = (int)rate;
    sfx_slots[id].loaded      = 1;

    next_spu_addr += dma_size;

    free(buf);
}

void sound_init(void) {
    SpuInit();

    int i;
    for (i = 0; i < SFX_COUNT; i++)
        load_vag((SfxID)i);
}

void sound_play(SfxID id) {
    if (id < 0 || id >= SFX_COUNT) return;
    SfxSlot *s = &sfx_slots[id];
    if (!s->loaded) return;

    int ch = sfx_channel(id);

    /* Stop the channel before reconfiguring */
    SpuSetKey(0, 1 << ch);

    SPU_CH_FREQ(ch)  = getSPUSampleRate(s->sample_rate);
    SPU_CH_ADDR(ch)  = getSPUAddr(s->spu_addr);
    SPU_CH_VOL_L(ch) = 0x3fff;
    SPU_CH_VOL_R(ch) = 0x3fff;
    /* 0x00ff / 0x0000 disables ADSR envelope — sample plays at full volume */
    SPU_CH_ADSR1(ch) = 0x00ff;
    SPU_CH_ADSR2(ch) = 0x0000;

    SpuSetKey(1, 1 << ch);
}

void sound_stop(SfxID id) {
    if (id < 0 || id >= SFX_COUNT) return;
    /* Same channel mapping as sound_play(): key the voice off so the sample
       stops immediately instead of playing out to its end. */
    int ch = sfx_channel(id);
    SpuSetKey(0, 1 << ch);
}
