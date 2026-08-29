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
    "\\SND\\NINURTA.VAG;1",
    "\\SND\\CURSOR.VAG;1",
    "\\SND\\SELECT.VAG;1",
    "\\SND\\BACK.VAG;1",
    "\\SND\\GATE.VAG;1",
    "\\SND\\GAS.VAG;1",
    "\\SND\\PULLIN.VAG;1",
    "\\SND\\HISS.VAG;1",
    "\\SND\\RUMBLE.VAG;1",
    "\\SND\\GRIND.VAG;1",
    NULL,                       /* SFX_RUMBLE_2: an alias, not a file          */
    NULL,                       /* SFX_RUMBLE_3                                */
    NULL,                       /* SFX_RUMBLE_4                                */
    NULL,                       /* SFX_RUMBLE_5                                */
    "\\SND\\HADDIE.VAG;1",
    "\\SND\\WOOSH.VAG;1",
    "\\SND\\MCHNEGH.VAG;1",
    "\\SND\\WATER.VAG;1",
};

/* Which bank(s) each effect belongs to — a MASK of SoundBank bits, so an effect
   can be in more than one and is then loaded once into each (see sound.h).
   SND_RESIDENT is 0, i.e. "no bank": those effects are allocated OUTSIDE the
   shared region and are never evicted, which is a real third case rather than a
   fourth flag.

   Read sound.h before adding to this list — banking an effect a room actually
   uses makes it silently disappear in that room, and the check that matters is
   world.c's placement block plus main.c's sound_bank_select mapping, not
   intuition. Zero, and so the default for anything omitted here, is
   SND_RESIDENT: see the note on SoundBank. */
#define SND_RESIDENT 0

static const uint8_t sfx_bank[SFX_COUNT] = {
    [SFX_SWING]      = SND_RESIDENT,   /* the boss's foot-slash windup uses it */
    [SFX_HURT]       = SND_RESIDENT,
    [SFX_PICKUP]     = SND_RESIDENT,
    [SFX_SMASH]      = SND_RESIDENT,   /* the boss's shockwave hit / parry     */
    [SFX_DOGBARK]    = SND_BANK_HOUSE,
    [SFX_AXEHIT]     = SND_RESIDENT,
    /* Re-cut from 22050 Hz to the 11025 house standard (20.1 KB -> 9.8 KB) to
       pay for SFX_RUMBLE's house copy below. Nothing times off its length. */
    [SFX_DOGDIE]     = SND_BANK_HOUSE,
    [SFX_UNLOCK]     = SND_RESIDENT,
    [SFX_DOOR]       = SND_RESIDENT,   /* plays on the way OUT of the courtyard,
                                          while the BOSS bank is still in       */
    [SFX_ZOMBIE]     = SND_BANK_HOUSE,
    [SFX_ZOMBIEDIE]  = SND_BANK_HOUSE,
    [SFX_DIE]        = SND_RESIDENT,
    [SFX_GR_SHOT]    = SND_RESIDENT,
    [SFX_GR_RELOAD]  = SND_RESIDENT,
    /* Both also in GARDEN: the Rafflesia borrows the writhe for its idle loop
       and the tentacle's death cry for its own (rafflesia.c). One copy per
       bank — 15.4 KB and 23.1 KB again in the garden's roomy 101 KB spare. */
    [SFX_TNTCL_WRTH] = SND_BANK_HOUSE | SND_BANK_GARDEN,
    [SFX_TNTCL_DIE]  = SND_BANK_HOUSE | SND_BANK_GARDEN,
    [SFX_STEP1]      = SND_RESIDENT,   /* the player walks in every room        */
    [SFX_STEP2]      = SND_RESIDENT,
    [SFX_SLAM]       = SND_RESIDENT,   /* the boss launching a shockwave        */
    [SFX_SPDR_WLK]   = SND_BANK_HOUSE | SND_BANK_GARDEN,  /* the flower's bite */
    [SFX_SPIT]       = SND_BANK_HOUSE,
    [SFX_MCHNE]      = SND_BANK_HOUSE,
    /* >>> THESE THREE WERE SND_RESIDENT AND DID NOT NEED TO BE. <<< They fire
       mid-fight and a bank swap is a CD read, so residency looked like the only
       way to guarantee they were loaded. It was not: they are played by
       src/rabisu.c and src/rabisu_boss.c alone, world.c places a Rabisu in the
       Garden Courtyard alone, and the BOSS bank is in for the whole of that
       room. The bank was already making the guarantee the residency was paying
       for — 47 KB of permanent SPU RAM, charged twice over (see sound.h). */
    [SFX_FIREBALL]   = SND_BANK_BOSS,
    [SFX_BOOM]       = SND_BANK_BOSS,
    [SFX_EXPLODE]    = SND_BANK_BOSS,
    [SFX_EMERGE]     = SND_BANK_BOSS,
    [SFX_DMNSPEAK]   = SND_BANK_BOSS,
    [SFX_RBS_SWING]  = SND_RESIDENT,   /* aliases a resident, so: resident      */
    [SFX_NINURTA]    = SND_BANK_INTRO, /* title screen only; see sound.h        */
    [SFX_CURSOR]     = SND_RESIDENT,   /* the menus open under every bank there */
    [SFX_SELECT]     = SND_RESIDENT,   /* is — title, rooms, courtyard — so all */
    [SFX_BACK]       = SND_RESIDENT,   /* three must be. See sound.h.           */
    /* The garden gate, in BOTH garden-side banks: it plays on the way out of
       the Garden Courtyard (boss bank) and out of Fountain Square and the
       Outside Catacombs (garden bank). It fits in neither the resident headroom
       nor the house bank — 17.9 KB against 6.6 KB spare in each — so two copies
       is what it takes. Full reasoning in sound.h. */
    [SFX_GATE]       = SND_BANK_BOSS | SND_BANK_GARDEN,
    [SFX_GAS]        = SND_BANK_GARDEN,  /* the Rafflesia's spore puff          */
    [SFX_PULL]       = SND_BANK_GARDEN,  /* ...and its grab (fireball reversed) */
    /* The Mushroom Head's scream. GARDEN only — it is 12.3 KB and the house
       bank, which is the largest and so sets `spare`, has 6.6 KB free. The
       garden bank runs to 105 KB of the region's 185, so this is free there.
       Placing a mushroom in a house room would leave it mute. */
    [SFX_HISS]       = SND_BANK_GARDEN,
    /* The Living Statue's teleport/death grind, 13.0 KB — and Hadad's, which is
       what put it in BOTH banks. He was GARDEN-only while he was only ever on
       the Rear Gate's plinth; once he could also be placed in the West Corridor,
       Reception and the Library (all HOUSE), a GARDEN-only tag would have left
       his arrival and death cue silently mute in exactly those rooms.
       >>> IT DID NOT FIT UNTIL dogdie WAS RE-CUT. <<< House is the largest bank
       and so sets `spare`, which was 6.6 KB against this clip's 13.0. dogdie was
       at 22050 Hz — off the 11025 house standard, with no reason recorded — and
       re-converting it to 11025 freed 9.8 KB, which pays for this copy and
       leaves 3.3 KB. House 178 KB -> 182 KB, garden unmoved at 127 KB, both
       inside the region's 185. Re-run the STEP 3 arithmetic in
       tools/ADDING_A_SOUND.txt before spending the rest. */
    [SFX_RUMBLE]     = SND_BANK_HOUSE | SND_BANK_GARDEN,
    /* The Rear Gate grinders' travel, 10.9 KB. GARDEN only, and unlike HISS and
       RUMBLE above it could not have been resident even if the headroom were
       there: a resident clip is charged twice, pushing bank_base up by its own
       size AND shrinking the region, which puts the 178 KB house bank 4.3 KB
       over. Garden goes 119 KB -> 130 KB of the region's 185. */
    [SFX_GRIND]      = SND_BANK_GARDEN,
    /* The four extra rumble voices. Aliases, so they cost no SPU bytes — but
       unlike SFX_RBS_SWING they alias a BANKED clip, so they are tagged with the
       source's own banks rather than SND_RESIDENT. That is what makes load_bank
       key their voices off and mark them unloaded on a bank swap, which is
       exactly right: the sample under them is about to move or disappear. The
       re-copy at the bottom of load_bank then restores them (or leaves them
       unloaded, in a bank RUMBLE is not in). See sound.h. */
    [SFX_RUMBLE_2]   = SND_BANK_HOUSE | SND_BANK_GARDEN,
    [SFX_RUMBLE_3]   = SND_BANK_HOUSE | SND_BANK_GARDEN,
    [SFX_RUMBLE_4]   = SND_BANK_HOUSE | SND_BANK_GARDEN,
    [SFX_RUMBLE_5]   = SND_BANK_HOUSE | SND_BANK_GARDEN,
    /* The death scene's roar and its spirit, 21.3 KB and 27.7 KB. GARDEN only —
       the Rear Gate is the only room either can sound in, and both are far too
       big for the 3.3 KB of resident headroom. Full reasoning in sound.h. */
    [SFX_HAD_DIE]    = SND_BANK_GARDEN,
    [SFX_WOOSH]      = SND_BANK_GARDEN,
    /* The Greenhouse vine curtain's grind: SFX_MCHNE re-cut for this bank,
       because the original is HOUSE-only and too big to copy here. See the
       block on SFX_MCHNE_GH in sound.h — it is what makes GARDEN the
       largest bank, so `spare` is now sized by this list. */
    [SFX_MCHNE_GH]   = SND_BANK_GARDEN,
    /* The Valve Puzzle's running water. GARDEN because all three rooms it plays
       in — Maze One, Fountain Square and the Rear Gate — are on that bank in
       main.c's sound_bank_select. It is HARDWARE-looped (see sound.h), so the
       eviction pass at the top of load_bank is what keeps it from reading the
       next bank's samples; valve_puzzle_area_sound() stops it properly on every
       room exit and does not rely on that. */
    [SFX_WATER]      = SND_BANK_GARDEN,
};

/* Which SPU voice a sound plays on. Short one-shot effects share a small pool
   (FIRST_VOICE + id%8). Anything long enough that a one-shot cutting it would
   be heard gets a voice of its own, clear of that pool:
     - the zombie groan, the tentacle writhe and the spider scuttle are
       continuous ambiences (the scuttle is HARDWARE-looped);
     - EMERGE (11 s), DMNSPEAK (8.6 s) and EXPLODE (5.4 s) are all far longer
       than the pool can protect: the footsteps and the weapon sounds share it
       and would chop them. EMERGE needs its own voice most of all, because
       unlike the other two it also plays DURING the fight, as the light beam's
       charge tell, where the player is free to swing and run over it;
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
    if (id == SFX_NINURTA)     return 9;    /* 4.8 s, and the intro's only sound */
    /* The Rafflesia's spore puff, off the pool on purpose. Its pool slot would
       be FIRST_VOICE + (33 % 8) = 2, shared with SFX_HURT — and the one moment
       the puff matters most is the frame a cloud lands on a player already
       standing in range, which fires HURT in the same frame and would cut it. */
    if (id == SFX_GAS)         return 13;
    /* The grab, likewise off the pool. It fires on the frame a flower seizes
       the player, which is a busy one — the haul starts, the player is very
       likely already gassed and taking damage — and its pool slot would be
       shared with PICKUP and SLAM. It is the cue that tells you what just
       happened to you, so nothing may cut it. */
    if (id == SFX_PULL)        return 14;
    /* The Mushroom Head's scream, off the pool for the same reason: it is 2 s
       long and it is a TELL — the leap comes out of the end of it, and a player
       who cannot hear it finish cannot read the wind-up. Its pool slot would be
       FIRST_VOICE + (35 % 8) = 4, shared with SFX_SMASH and SFX_DIE, and the
       scream is followed within the second by a lunge that may well kill. 15 is
       the last of the free 13..15 block. */
    if (id == SFX_HISS)        return 15;
    /* The Living Statue's grind, 2.12 s, and the ONLY cue that it has moved —
       the player is meant to hear it appear behind them. Its pool slot would be
       FIRST_VOICE + (36 % 8) = 5, shared with SFX_GR_SHOT, which is resident and
       which the player is free to fire while running away; a gunshot cutting the
       teleport cue would delete the enemy's only tell.
       Voices 13..15 are spoken for, so it SHARES voice 9 with SFX_NINURTA. That
       still costs nothing now the clip is in the HOUSE bank as well as GARDEN:
       NINURTA is SND_BANK_INTRO, which main.c selects on the TITLE SCREEN and
       nowhere else, while HOUSE, GARDEN and BOSS are the three a room can ask
       for. INTRO is never loaded alongside any of them, so the intro line and
       this grind can never sound in the same place whatever bank a room is on.
       Widening the mask again is the thing to re-check here — a second GARDEN
       or HOUSE clip landing on voice 9 would cut this one. */
    if (id == SFX_RUMBLE)      return 9;
    /* The quake's four extra rumble voices, borrowed from sounds that cannot be
       in a HOUSE room: GAS, PULL and HISS are the garden's flower and mushroom,
       EMERGE is the boss's. The quake only ever runs in the Attic Exit and the
       East Hall, both HOUSE rooms, so those four are idle every time it does.
       Full reasoning on SFX_RUMBLE_2 in sound.h — and read it before playing a
       rumble anywhere else. */
    if (id == SFX_RUMBLE_2)    return 13;   /* SFX_GAS's    (GARDEN) */
    if (id == SFX_RUMBLE_3)    return 14;   /* SFX_PULL's   (GARDEN) */
    if (id == SFX_RUMBLE_4)    return 15;   /* SFX_HISS's   (GARDEN) */
    if (id == SFX_RUMBLE_5)    return 19;   /* SFX_EMERGE's (BOSS)   */
    /* The grinders' travel. Off the pool because it runs 1.76 s and is played
       three times WITHOUT a gap — the sequence is 5.3 s long and the player is
       free to walk, shoot and reload right through it. Its pool slot would be
       FIRST_VOICE + (37 % 8) = 6, shared with SFX_AXEHIT and SFX_GR_RELOAD, and
       a reload is exactly the sort of thing a player does while watching a door
       close. VOICE 0, which FIRST_VOICE = 1 has always left out of the pool and
       which nothing else in the game has ever claimed; 13..15 are spoken for by
       the flower and the mushroom, and 9 is shared by the intro line and the
       Living Statue — and the statue is GARDEN too, so it could sound in this
       very room the day one is placed here. */
    if (id == SFX_GRIND)       return 0;
    /* The death scene's two clips, both off the pool: they are 3.45 s and
       4.49 s, they are the whole soundtrack of the moment, and nothing may cut
       either. Their pool slots would be FIRST_VOICE + (42 % 8) = 3 and
       (43 % 8) = 4 — SFX_SMASH and SFX_DIE among others — and the grinders'
       three-play travel is running under both of them.
       >>> BORROWED VOICES, LEGAL FOR THE REASON SFX_RUMBLE_2's ARE. <<< 20 is
       SFX_DMNSPEAK's and 22 is SFX_BOOM's; BOTH are BOSS-bank now, so both are
       guaranteed idle in a GARDEN room. (BOOM was resident-but-boss-only when
       this was written, which was a weaker guarantee than the one it now has.) Voices 13..15 and 9 are NOT available
       here the way they are to the quake: those belong to the flower, the
       mushroom and the Living Statue, all three of which are garden-bank
       monsters and any of which may one day be placed in this very corridor. */
    if (id == SFX_HAD_DIE)     return 20;   /* SFX_DMNSPEAK's (BOSS)          */
    if (id == SFX_WOOSH)       return 22;   /* SFX_BOOM's (BOSS bank)           */
    /* The menu blips, one voice each out of the free 10..15. They are short
       enough for the pool, but a menu is the one place the player fires sounds
       back to back at speed, and in the pool a cursor run would cut the confirm
       that ends it (BACK, at 1.23 s, would be cut by almost anything). Off the
       pool they also stay clear of the footsteps and the weapon sounds, which
       keep playing under the pause menu's frozen room. */
    /* THE RUNNING WATER, and it needs a voice NOTHING ELSE CAN TOUCH. It is
       hardware-looped and stays keyed on for as long as the player is in one of
       its three rooms, so any other effect sharing its voice would not merely
       cut it — it would end the loop for good and the drain would go silent
       until the next room change. The pool is therefore out on principle, not
       on length.
       19 is SFX_EMERGE's, and borrowing it is legal for the reason SFX_HAD_DIE
       borrows SFX_DMNSPEAK's: EMERGE is BOSS-bank, fired only by the Rabisu
       (src/rabisu.c and src/rabisu_boss.c), and the boss bank is loaded in the
       Garden Courtyard alone — a room the water never plays in. Voices 13..15
       and 9 are NOT available here, for the same reason they are not available
       to the Hadad's death: they belong to garden-bank monsters that may be
       placed in any of these three rooms. */
    if (id == SFX_WATER)       return 19;   /* SFX_EMERGE's (BOSS-bank only)  */
    if (id == SFX_CURSOR)      return 10;
    if (id == SFX_SELECT)      return 11;
    if (id == SFX_BACK)        return 12;
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

    /* `& b`, not `== b`: sfx_bank is a mask, so an effect listed in several
       banks is loaded afresh into each one. Its address therefore DIFFERS
       between banks, which is exactly why the eviction pass above must mark
       every banked slot unloaded first — a stale spu_addr from the previous
       bank would point at whatever this one laid down in its place. */
    for (i = 0; i < SFX_COUNT; i++)
        if (sfx_bank[i] & b) addr += load_vag_at((SfxID)i, addr, limit);

    /* The quake's rumble aliases. They point at a BANKED sample, so unlike
       sound_init's RBS_SWING copy this has to be redone after every load: the
       address RUMBLE landed at differs bank to bank, and in a bank without it
       the source slot is `loaded = 0` and the copy correctly makes these mute
       too. Struct copy, so address, rate and loaded all move together. */
    sfx_slots[SFX_RUMBLE_2] = sfx_slots[SFX_RUMBLE];
    sfx_slots[SFX_RUMBLE_3] = sfx_slots[SFX_RUMBLE];
    sfx_slots[SFX_RUMBLE_4] = sfx_slots[SFX_RUMBLE];
    sfx_slots[SFX_RUMBLE_5] = sfx_slots[SFX_RUMBLE];

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

       As of writing: residents end at 0x46150, so the region is 0x39EB0
       (231.7 KB). The house bank uses 158.8 KB of it, the boss bank 185.9 KB,
       the garden bank 173.1 KB, the intro bank 21.6 KB and the ASAG bank 0 —
       it is empty until the fight has clips. `spare` (the region less the
       LARGEST bank) is 45.8 KB, up from 12.2 KB: moving the Rabisu's
       FIREBALL/BOOM/EXPLODE off the residents and into the boss bank handed
       47 KB back to every bank at once. See sound.h.

       >>> RE-RUN THE ARITHMETIC, DO NOT QUOTE THESE. <<< STEP 3 of
       tools/ADDING_A_SOUND.txt is the script; a bank that overruns is SILENT,
       not broken — load_vag_at drops the clip and it is mute forever. */
    bank_base  = next_spu_addr;
    bank_bytes = SPU_RAM_END - bank_base;

    /* The INTRO bank, not the house one, because the title screen is where the
       game starts and the opening sequence's voice is the first thing that has
       to be ready. Every route out of the title asks for the bank it actually
       needs — main.c's title-exit hook does it for the delivery area, and
       STATE_LOADING does it for a Load Game or a debug level-select jump — so
       nothing reaches a room with this one still in. */
    load_bank(SND_BANK_INTRO);
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
