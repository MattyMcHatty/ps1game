#include <stdint.h>
#include <stdlib.h>
#include <psxspu.h>
#include <psxcd.h>
#include "cdaudio.h"
#include "sound.h"

/* SPU RAM layout — first 0x1000 reserved, SpuInit uploads dummy at 0x1000.
   The whole layout, and why there is a bank region at all, is in sound.h. */
#define ALLOC_START 0x1010
#define SPU_RAM_END 0x80000
#define FIRST_VOICE 1

typedef struct {
    int spu_addr;
    int sample_rate;
    int loaded;
} SfxSlot;

static SfxSlot sfx_slots[SFX_COUNT];
static int     next_spu_addr = ALLOC_START;

/* The shared region: where it starts, how big it is, and what is in it.
   bank_base is fixed by sound_init once the residents are down; bank_bytes is
   the extent the HOUSE bank occupied, which is the larger of the two and so
   the region's true size. */
static int       bank_base   = 0;
static int       bank_bytes  = 0;
static SoundBank bank_loaded = SND_BANK_HOUSE;

/* VAGs live in the \SND subdirectory (see disc.xml): keeping them out of the
   root directory stops it spilling into a second sector, which the PS1 boot ROM
   can't follow to find SYSTEM.CNF (the disc would freeze at the BIOS logo). */
static const char *sfx_files[SFX_COUNT] = {
    "\\SND\\SWING.VAG;1",
    "\\SND\\HURT.VAG;1",
    "\\SND\\PICKUP.VAG;1",
    "\\SND\\SMASH.VAG;1",
    "\\SND\\DOGBARK.VAG;1",
    "\\SND\\AXEHIT.VAG;1",
    "\\SND\\DOGDIE.VAG;1",
    "\\SND\\UNLOCK.VAG;1",
    "\\SND\\DROPEN.VAG;1",
    "\\SND\\ZOMBIE.VAG;1",
    "\\SND\\ZOMBDIE.VAG;1",
    "\\SND\\DIE.VAG;1",
    "\\SND\\GRSHOT.VAG;1",
    "\\SND\\GRRELOAD.VAG;1",
    "\\SND\\TNTCLWRT.VAG;1",
    "\\SND\\TNTCLDIE.VAG;1",
    "\\SND\\STEP1.VAG;1",
    "\\SND\\STEP2.VAG;1",
    "\\SND\\SLAM.VAG;1",
    "\\SND\\SPDRWLK.VAG;1",
    "\\SND\\SPIT.VAG;1",
    "\\SND\\MCHNE.VAG;1",
    "\\SND\\FIREBALL.VAG;1",
    "\\SND\\BOOM.VAG;1",
    "\\SND\\EXPLODE.VAG;1",
    "\\SND\\EMERGE.VAG;1",
    "\\SND\\DMNSPEK.VAG;1",
    NULL,                       /* SFX_RBS_SWING: an alias, not a file         */
};

/* Which bank each effect belongs to. SND_RESIDENT is spelled as a third value
   here rather than a flag because "resident" is a real third case: those
   effects are allocated OUTSIDE the shared region and are never evicted.

   The nine SND_BANK_HOUSE entries are the monster effects, and they are the
   nine because the Garden Courtyard places no monster but the boss (world.c).
   Read sound.h before adding to this list — banking an effect a room actually
   uses makes it silently disappear in that room. Zero, and so the default for
   anything omitted here, is SND_RESIDENT: see the note on SoundBank. */
#define SND_RESIDENT 0

static const uint8_t sfx_bank[SFX_COUNT] = {
    [SFX_SWING]      = SND_RESIDENT,   /* the boss's foot-slash windup uses it */
    [SFX_HURT]       = SND_RESIDENT,
    [SFX_PICKUP]     = SND_RESIDENT,
    [SFX_SMASH]      = SND_RESIDENT,   /* the boss's shockwave hit / parry     */
    [SFX_DOGBARK]    = SND_BANK_HOUSE,
    [SFX_AXEHIT]     = SND_RESIDENT,
    [SFX_DOGDIE]     = SND_BANK_HOUSE,
    [SFX_UNLOCK]     = SND_RESIDENT,
    [SFX_DOOR]       = SND_RESIDENT,   /* plays on the way OUT of the courtyard,
                                          while the BOSS bank is still in       */
    [SFX_ZOMBIE]     = SND_BANK_HOUSE,
    [SFX_ZOMBIEDIE]  = SND_BANK_HOUSE,
    [SFX_DIE]        = SND_RESIDENT,
    [SFX_GR_SHOT]    = SND_RESIDENT,
    [SFX_GR_RELOAD]  = SND_RESIDENT,
    [SFX_TNTCL_WRTH] = SND_BANK_HOUSE,
    [SFX_TNTCL_DIE]  = SND_BANK_HOUSE,
    [SFX_STEP1]      = SND_RESIDENT,   /* the player walks in every room        */
    [SFX_STEP2]      = SND_RESIDENT,
    [SFX_SLAM]       = SND_RESIDENT,   /* the boss launching a shockwave        */
    [SFX_SPDR_WLK]   = SND_BANK_HOUSE,
    [SFX_SPIT]       = SND_BANK_HOUSE,
    [SFX_MCHNE]      = SND_BANK_HOUSE,
    [SFX_FIREBALL]   = SND_RESIDENT,   /* the three below fire mid-fight, and a */
    [SFX_BOOM]       = SND_RESIDENT,   /* bank swap is a CD read — it cannot    */
    [SFX_EXPLODE]    = SND_RESIDENT,   /* happen inside a room. See sound.h.    */
    [SFX_EMERGE]     = SND_BANK_BOSS,
    [SFX_DMNSPEAK]   = SND_BANK_BOSS,
    [SFX_RBS_SWING]  = SND_RESIDENT,   /* aliases a resident, so: resident      */
};

/* Which SPU voice a sound plays on. Short one-shot effects share a small pool
   (FIRST_VOICE + id%8). Anything long enough that a one-shot cutting it would
   be heard gets a voice of its own, clear of that pool:
     - the zombie groan, the tentacle writhe and the spider scuttle are
       continuous ambiences (the scuttle is HARDWARE-looped);
     - EMERGE (11 s), DMNSPEAK (8.6 s) and EXPLODE (5.4 s) each play under a
       cutscene the player cannot interrupt, but the footsteps and the weapon
       sounds share the pool and would chop them;
     - BOOM gets one so the beam's detonations are cut only by the NEXT
       detonation, which is the intended read (see sound.h).
   FIREBALL stays in the pool deliberately: it is a 1.1 s one-shot, and the
   only other id in its slot (DOGDIE) is banked out wherever a Rabisu is. */
static int sfx_channel(SfxID id) {
    if (id == SFX_ZOMBIE)      return 16;
    if (id == SFX_TNTCL_WRTH)  return 17;   /* continuous loop; own voice too */
    if (id == SFX_SPDR_WLK)    return 18;   /* ditto: the spider scuttle loop  */
    if (id == SFX_EMERGE)      return 19;
    if (id == SFX_DMNSPEAK)    return 20;
    if (id == SFX_EXPLODE)     return 21;
    if (id == SFX_BOOM)        return 22;
    if (id == SFX_RBS_SWING)   return 23;   /* clear of SFX_SWING's pool slot */
    /* SFX_MCHNE is long (2.8 s) but stays in the one-shot pool on purpose: it
       only ever plays during the piano puzzle, which owns the screen in a room
       with no enemies, and the two other ids that share its slot (AXEHIT,
       GR_RELOAD) are weapon sounds the puzzle locks out. */
    return FIRST_VOICE + (id % 8);
}

/* Read one VAG off the disc and DMA it into SPU RAM at `addr`. Returns the
   number of SPU bytes consumed (0 if it could not be loaded), so the caller
   can lay the next one down straight after it.

   `limit` is the first address this must not touch. For the residents that is
   the end of SPU RAM; for a bank load it is the end of the shared region, and
   overrunning it would scribble over whatever follows rather than failing. */
static int load_vag_at(SfxID id, int addr, int limit) {
    CdlFILE file;
    sfx_slots[id].loaded = 0;
    if (!sfx_files[id]) return 0;   /* an alias; sound_init wires it up */
    if (!CdSearchFile(&file, (char *)sfx_files[id])) return 0;

    int sectors = (file.size + 2047) / 2048;
    void *buf   = malloc(sectors * 2048);
    if (!buf) return 0;

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

    /* Out of room. Bail rather than corrupt a neighbour: the slot stays
       unloaded, so this effect goes silent and nothing else is harmed. */
    if (addr + dma_size > limit) { free(buf); return 0; }

    /* Convert to KSEG1 (uncached) so SPU DMA reads physical RAM directly,
       avoiding cache coherency issues with the preceding CdRead DMA. */
    const uint32_t *audio_uncached =
        (const uint32_t *)((uint32_t)audio | 0xA0000000);

    SpuSetTransferMode(SPU_TRANSFER_BY_DMA);
    SpuSetTransferStartAddr(addr);
    SpuWrite(audio_uncached, dma_size);
    SpuIsTransferCompleted(SPU_TRANSFER_WAIT);

    sfx_slots[id].spu_addr    = addr;
    sfx_slots[id].sample_rate = (int)rate;
    sfx_slots[id].loaded      = 1;

    free(buf);
    return dma_size;
}

/* Fill the shared region with `b`. Everything the region held is evicted
   first — voices keyed off AND slots marked unloaded — because the samples
   they point at are about to be overwritten. The scuttle loop is the one that
   makes this mandatory rather than tidy: it is hardware-looped and would go on
   reading the boss's speech out of the same addresses forever. */
static void load_bank(SoundBank b) {
    int i, addr = bank_base;
    int limit   = bank_base + bank_bytes;

    for (i = 0; i < SFX_COUNT; i++) {
        if (sfx_bank[i] == SND_RESIDENT) continue;
        SpuSetKey(0, 1 << sfx_channel((SfxID)i));
        sfx_slots[i].loaded = 0;
    }

    for (i = 0; i < SFX_COUNT; i++)
        if (sfx_bank[i] == b) addr += load_vag_at((SfxID)i, addr, limit);

    bank_loaded = b;
}

void sound_init(void) {
    SpuInit();

    int i;
    /* The residents first, so they occupy one unbroken run from ALLOC_START
       and the region that follows them is likewise unbroken. */
    for (i = 0; i < SFX_COUNT; i++)
        if (sfx_bank[i] == SND_RESIDENT)
            next_spu_addr += load_vag_at((SfxID)i, next_spu_addr, SPU_RAM_END);

    /* The aliases: same sample, same rate, different voice. Copied AFTER the
       resident pass so the source slot is populated, and costing nothing in
       SPU RAM because both slots point at the one upload. */
    sfx_slots[SFX_RBS_SWING] = sfx_slots[SFX_SWING];

    /* Everything above this line is permanent; everything below is the shared
       region, and nothing follows the region — so it simply gets ALL the SPU
       RAM that is left. Sizing it to the house bank instead would be a second
       number to keep in step with the .vag files for no gain: there is nothing
       past it to protect, and the only thing worth catching is a bank that
       overruns SPU RAM itself, which load_vag_at's limit does.

       As of writing: residents end at 0x4F5D0, so the region is 0x30A30
       (199 KB). The house bank uses 178 KB of it and the boss bank 121 KB. */
    bank_base  = next_spu_addr;
    bank_bytes = SPU_RAM_END - bank_base;
    load_bank(SND_BANK_HOUSE);
}

void sound_play(SfxID id) {
    if (id < 0 || id >= SFX_COUNT) return;
    SfxSlot *s = &sfx_slots[id];
    /* Not loaded means banked out (or missing from the disc). Silence is the
       right answer: the caller is a monster that cannot be in this room. */
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

void sound_bank_select(SoundBank bank) {
    if (!bank_base || bank == bank_loaded) return;

    /* The drive cannot do both at once: a CdRead issued while CD-DA is
       streaming hangs it (see cdaudio_suspend). Suspend/resume are no-ops if
       nothing is playing, which is the usual case here — every transition into
       or out of the Garden Courtyard stops the music anyway. */
    cdaudio_suspend();
    load_bank(bank);
    cdaudio_resume();
}
