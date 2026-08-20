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
 *              |     BOOM, EXPLODE and the |
 *              |     three menu blips      |
 *     0x51C10  +---------------------------+  <- bank_base
 *              | THE BANK REGION           |  185 KB, all that is left:
 *              |   HOUSE (178 KB) *or*     |  nothing follows it, so it takes
 *              |   BOSS  (121 KB)          |  the whole tail and the slack
 *     0x80000  +---------------------------+  lands in whichever bank is in
 *
 *   Every effect carries a bank tag (see sfx_bank[] in sound.c):
 *     SND_RESIDENT    - loaded once at startup, playable in every room.
 *     SND_BANK_HOUSE  - the monster effects. Loaded at startup, evicted while
 *                       another bank is in.
 *     SND_BANK_BOSS   - the Rabisu's own two clips plus the gate. Only ever in
 *                       the Garden Courtyard.
 *     SND_BANK_GARDEN - the outdoor rooms: the gate again, plus the Rafflesia's
 *                       four. Fountain Square and the Outside Catacombs.
 *
 *   The tag is a MASK, not a single value — an effect can be in more than one
 *   bank, at the cost of one copy in each. See the SoundBank enum below.
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
    /* ---- The menu blips. RESIDENT, and they have to be: a menu opens on the
       title screen (INTRO bank in), in every room (HOUSE bank in) and in the
       Garden Courtyard (BOSS bank in), so any bank tag would make them silent
       somewhere the player can still move a cursor. They are 8000 Hz rather
       than the house standard 11025 to afford exactly that — 9.8 KB of the
       resident headroom against 13.3 KB, leaving ~6.7 KB spare. A menu blip is
       short and percussive and loses almost nothing to the lower rate; a longer
       clip would not have fitted at all.

       Each has a voice of its own (see sfx_channel): a menu is the one place
       where the player generates sounds faster than anything else in the game,
       and on shared voices a fast cursor run would chop the confirm that ends
       it. They cost nothing to separate — voices 10..12 were free. ---- */
    SFX_CURSOR     = 29, /* the cursor steps between options. 0.19 s          */
    SFX_SELECT     = 30, /* an option is chosen / a screen is confirmed. 0.71 s.
                            Suppressed where the confirm already has an outcome
                            sound of its own in the same frame — the puzzles'
                            UNLOCK/PICKUP/SLAM — so nothing ever layers.       */
    SFX_BACK       = 31, /* backing out of a screen or cancelling. 1.23 s     */
    /* The wrought-iron garden gate between the Garden Courtyard and Fountain
       Square, played by DOOR_PANEL_GATE's transition. 2.90 s at 11025 Hz, and
       door_anim.c's GATE_SWING_FRAMES is cut to exactly that — retrim this clip
       and that constant has to move with it.

       BANKED, and in TWO banks: BOSS | GARDEN. It is 17.9 KB and the resident
       headroom is 6.6 KB, so it cannot be resident; the house bank has exactly
       the same 6.6 KB spare, so it cannot go there either. It now joins the
       gate's three rooms the honest way — a copy in the boss bank for the
       Garden Courtyard and a copy in the garden bank for Fountain Square and
       the Outside Catacombs.

       This used to be BOSS only, which forced Fountain Square onto the boss
       bank purely to hear its own gate, and THAT cost the square every monster
       sound in the game. The garden bank exists to undo that. */
    SFX_GATE       = 32,
    /* The Rafflesia exhaling a cloud of spores. 0.75 s at 11025 Hz — 4.7 KB,
       the smallest effect in the game. GARDEN bank; it is the only one of the
       flower's four sounds that is not borrowed from a house monster. */
    SFX_GAS        = 33,
    /* The Rafflesia seizing the player, on the frame its grip is claimed. This
       is fireball.wav PLAYED BACKWARDS (ffmpeg -af areverse) — a fireball's
       decay run in reverse is a rising suck, which is exactly the read wanted,
       and it costs 6.8 KB rather than a new recording. Same length and rate as
       the original, 1.07 s at 11025 Hz. GARDEN bank. */
    SFX_PULL       = 34,
    SFX_COUNT      = 35,
} SfxID;

/* Which set of effects the shared SPU region currently holds.
 *
 * >>> THESE ARE BIT FLAGS, AND sfx_bank[] IS A MASK. <<< An effect may belong
 * to SEVERAL banks — it is then loaded into each of them, at whatever address
 * that bank's pass happens to reach, and costs its own SPU bytes once per bank.
 * That is what lets the garden gate live in both the BOSS and GARDEN banks (it
 * plays on the way out of rooms on both), and the tentacle writhe, the tentacle
 * death and the spider scuttle live in both HOUSE and GARDEN (the Rafflesia
 * borrows all three). Duplicating a sample is far cheaper than the alternative
 * of making it resident, which would cost permanent RAM in every room.
 *
 * Zero — the value an effect left out of sound.c's sfx_bank[] would get — still
 * means SND_RESIDENT: a missed entry then costs permanent SPU RAM, which the
 * startup arithmetic catches, instead of going silently mute in one room. */
typedef enum {
    SND_BANK_HOUSE = 1,   /* the monsters — the house, and the Garden Stairs    */
    SND_BANK_BOSS  = 2,   /* the Rabisu's reveal — the Garden Courtyard only     */
    /* The outdoor rooms: Fountain Square and the Outside Catacombs. Added when
       the Rafflesia needed sounds in a room that was on the BOSS bank purely to
       reach SFX_GATE, and so had no monster effects at all. It holds the gate
       plus the flower's four, 83.9 KB against the region's 185 — the roomiest
       bank in the game.

       >>> THE GARDEN COURTYARD IS NOT ON IT AND CANNOT BE. <<< It needs the
       boss bank for the fight, and the flower's four sounds are 66.1 KB against
       the 45.8 KB the boss bank has spare. A rafflesia placed in the courtyard
       would be mute. The Garden Stairs is deliberately left on HOUSE so that
       house monsters remain placeable there; the price is that a rafflesia on
       the stairs would be silent on SFX_GAS alone (its other three are in both
       banks). */
    SND_BANK_GARDEN = 4,
    SND_BANK_INTRO = 8,   /* the opening sequence's voice line. Loaded by
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
