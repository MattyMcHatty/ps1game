#ifndef SOUND_H
#define SOUND_H

#include <stdint.h>

/* -----------------------------------------------------------------------
 * Sound effects, and the SPU-RAM TIMESHARE that lets there be more of them
 * than fit at once.
 *
 * THE PROBLEM
 *   The SPU has 512 KB of sample RAM and no more. The first 0x1000 is the
 *   CD-DA capture area, so 0x1010 upward is ours. The resident effects below
 *   already fill ~325 KB of it, and the Rabisu's five clips want another
 *   172 KB. There is nowhere near that much left. This is the same squeeze
 *   VRAM is under, and it gets the same answer: a shared region that holds
 *   one BANK at a time, swapped on a room transition.
 *
 * THE LAYOUT
 *
 *     0x00000  +---------------------------+
 *              | CD-DA capture + SpuInit   |  reserved
 *     0x01010  +---------------------------+
 *              | RESIDENT effects          |  always loaded, never moves
 *              |  ...including FIREBALL,   |
 *              |     BOOM and EXPLODE      |
 *     0x4F5D0  +---------------------------+  <- bank_base
 *              | THE BANK REGION           |  199 KB, all that is left:
 *              |   HOUSE (178 KB) *or*     |  nothing follows it, so it takes
 *              |   BOSS  (121 KB)          |  the whole tail and the slack
 *     0x80000  +---------------------------+  lands in whichever bank is in
 *
 *   Every effect carries a bank tag (see sfx_bank[] in sound.c):
 *     SND_RESIDENT   - loaded once at startup, playable in every room.
 *     SND_BANK_HOUSE - the monster effects. Loaded at startup, evicted while
 *                      the boss bank is in.
 *     SND_BANK_BOSS  - the Rabisu's own two clips. Only ever in the Garden
 *                      Courtyard, which is the only place they are played.
 *
 * WHY THESE THREE ARE RESIDENT AND NOT BANKED
 *   FIREBALL, BOOM and EXPLODE fire DURING the fight, and a bank swap is a CD
 *   read — it cannot happen mid-room. They are small enough (47 KB together)
 *   to live in what was previously the spare tail, so they do. Only EMERGE and
 *   DMNSPEAK, which are enormous (70 KB and 54 KB), are worth banking. Note
 *   that EMERGE is no longer reveal-only — it is also the light beam's charge
 *   tell, so it now plays mid-fight. That is safe for exactly one reason: the
 *   boss bank is loaded for the whole of the Garden Courtyard, cutscene and
 *   fight alike, and the fight cannot happen anywhere else.
 *
 * WHY THE HOUSE BANK IS THE MONSTERS
 *   world.c places nothing in the Garden Courtyard but the Rabisu itself — no
 *   zombies, dogs, spiders, tentacles, crates or pickups. Those nine effects
 *   are therefore dead weight in that one room, and they are the only nine
 *   that are. Everything the boss fight itself needs — SWING, HURT, DIE,
 *   AXEHIT, SMASH, SLAM, the footsteps and the door — is RESIDENT and stays
 *   put. >>> Before banking anything else, check it against the room. <<<
 *   A banked-out effect does not fall back to silence gracefully anywhere
 *   except in the room it was banked out for: sound_play() simply returns.
 *
 * THE SWAP IS A CD READ
 *   Unlike texmgr, which keeps a RAM copy of every streamed texture, banks are
 *   re-read from \SND\ on the transition. 178 KB of resident RAM copies is not
 *   affordable next to the room meshes; half a second of drive time behind the
 *   door fade is. sound_bank_select() suspends CD-DA around the read for the
 *   reason cdaudio_suspend() documents: a CdRead issued while the drive is
 *   streaming audio hangs it.
 *
 * ADDING AN EFFECT: see tools/ADDING_A_SOUND.txt.
 * ----------------------------------------------------------------------- */

typedef enum {
    SFX_SWING   = 0,
    SFX_HURT    = 1,
    SFX_PICKUP  = 2,
    SFX_SMASH   = 3,
    SFX_DOGBARK = 4,
    SFX_AXEHIT  = 5,   /* crucifaxe connects with an enemy (non-fatal hit) */
    SFX_DOGDIE  = 6,
    SFX_UNLOCK  = 7,
    SFX_DOOR     = 8,   /* double-door open/close, used by the level transition */
    SFX_ZOMBIE   = 9,   /* zombie groan, looped while a zombie is alert */
    SFX_ZOMBIEDIE = 10, /* zombie death */
    SFX_DIE       = 11, /* player death */
    SFX_GR_SHOT   = 12, /* grave-olver gunshot */
    SFX_GR_RELOAD = 13, /* grave-olver reload */
    SFX_TNTCL_WRTH = 14, /* tentacle writhe, looped while a tentacle is alert */
    SFX_TNTCL_DIE  = 15, /* tentacle death — also the spider's (see spider.c) */
    SFX_STEP1      = 16, /* footstep A, used by the conservatory<->2F stair transition */
    SFX_STEP2      = 17, /* footstep B */
    SFX_SLAM       = 18, /* drawers slam shut (trick-drawers puzzle fail); also
                            the Rabisu launching a shockwave */
    SFX_SPDR_WLK   = 19, /* spider scuttle, HARDWARE-looped while any spider walks */
    SFX_SPIT       = 20, /* spider fires a web (one-shot, once per web)            */
    SFX_MCHNE      = 21, /* grinding machinery — the piano room's sinking bookcase.
                            2.80 s at 11025 Hz; piano_props.c plays it twice back
                            to back and times the descent to match. */
    /* ---- The Rabisu. See the bank note above for which of these are resident
       and why. ---- */
    SFX_FIREBALL   = 22, /* a fireball leaves the boss's chest (1.1 s)          */
    SFX_BOOM       = 23, /* one poly of the light-beam path erupting (1.1 s).
                            Retriggered every RBS_BEAM_STEP (0.3 s) as the beam
                            walks, so each one is cut short by the next and the
                            attack reads as a chain of detonations — only the
                            last plays its tail out. */
    SFX_EXPLODE    = 24, /* the death lights come up and the body starts coming
                            apart (RBE_D_BURN). 5.374 s, and the burn plus the
                            fade are cut to exactly that: the lights are lit for
                            the length of this clip. Retrim it and see
                            RBE_T_D_BURN.                                       */
    SFX_EMERGE     = 25, /* BANKED. Light being hauled up out of the ground.
                            11.1 s, which covers the reveal's 3 s of lights plus
                            its 5 s rise. Also the light beam's charge tell,
                            where it is deliberately cut after ~1.5 s by the
                            first poly igniting.                                */
    SFX_DMNSPEAK   = 26, /* BANKED. One line of scripture; played once per line,
                            the second retriggering over the first.             */
    /* SFX_SWING's sample on a voice of its own. Not a second clip: no file on
       the disc, no second copy in SPU RAM, just an alias set up by sound_init.
       It exists because the boss's foot-slash wind-up and the player's own axe
       swing are the SAME sound, and on one voice the player's swing silences
       the tell for the only attack that cannot be sidestepped — the parry
       window would then be pure guesswork whenever the player was mid-swing. */
    SFX_RBS_SWING  = 27,
    SFX_NINURTA    = 28, /* BANKED (intro). The Order of Ninurta line, over the
                            white flash that opens the game. 4.82 s at 8000 Hz,
                            deliberately band-limited and 6-bit crushed — see
                            sounds/crush_wav.py. Banked rather than resident
                            because 21.6 KB does not fit the ~16 KB of resident
                            headroom, and it only ever plays on the title. */
    SFX_COUNT      = 29,
} SfxID;

/* Which set of effects the shared SPU region currently holds. Numbered from 1
   so that 0 — the value an effect left out of sound.c's sfx_bank[] would get —
   means SND_RESIDENT: a missed entry then costs permanent SPU RAM, which the
   startup arithmetic catches, instead of going silently mute in one room. */
typedef enum {
    SND_BANK_HOUSE = 1,   /* the monsters — every room but the Garden Courtyard */
    SND_BANK_BOSS  = 2,   /* the Rabisu's reveal — the Garden Courtyard only     */
    SND_BANK_INTRO = 3,   /* the opening sequence's voice line. Loaded by
                             intro_start() and gone by the time any room is
                             entered: every path out of the intro reaches
                             main.c's title-exit hook, which asks for the house
                             bank back. Safe to swap in from the title because
                             nothing is playing there yet — this is the one bank
                             load that is NOT behind a door transition, and it
                             is legal for the same reason those are: the drive
                             is idle. */
} SoundBank;

void sound_init(void);
void sound_play(SfxID id);
void sound_stop(SfxID id);

/* Swap the shared region over to `bank`. A no-op if it is already loaded, so
   this can be called unconditionally on every room transition — which is what
   main.c's STATE_LOADING does, keyed on pending_area.

   >>> ONLY SAFE DURING A ROOM TRANSITION. <<< It stops CD-DA, issues blocking
   CdReads and keys off every voice in the region first (the spider scuttle is
   hardware-looped and would otherwise read the boss's samples forever). Costs
   roughly half a second of drive time, which is why it lives behind the door
   animation's black screen. */
void sound_bank_select(SoundBank bank);

#endif
