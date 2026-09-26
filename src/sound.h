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
 *   fill ~279 KB of it, and the monsters and set-pieces want far more than the
 *   rest between them. This is the same squeeze
 *   VRAM is under, and it gets the same answer: a shared region that holds
 *   one BANK at a time, swapped on a room transition.
 *
 * THE LAYOUT
 *
 *     0x00000  +---------------------------+
 *              | CD-DA capture + SpuInit   |  reserved
 *     0x01010  +---------------------------+
 *              | RESIDENT effects          |  always loaded, never moves:
 *              |  the player's own kit and |  the weapons, the footsteps, the
 *              |  the three menu blips     |  hurt/die pair, the door, and the
 *              |                           |  cursor/select/back triple
 *     0x46150  +---------------------------+  <- bank_base
 *              | THE BANK REGION           |  232 KB, all that is left:
 *              |   HOUSE  (159 KB) *or*    |  nothing follows it, so it takes
 *              |   BOSS   (186 KB) *or*    |  the whole tail and the slack
 *              |   GARDEN (173 KB) *or*    |  lands in whichever bank is in
 *              |   ASAG   (  0 KB)         |
 *     0x80000  +---------------------------+
 *
 *   Every effect carries a bank tag (see sfx_bank[] in sound.c):
 *     SND_RESIDENT    - loaded once at startup, playable in every room.
 *     SND_BANK_HOUSE  - the monster effects. Loaded at startup, evicted while
 *                       another bank is in.
 *     SND_BANK_BOSS   - the Rabisu's five clips plus the gate. Only ever in
 *                       the Garden Courtyard.
 *     SND_BANK_GARDEN - the outdoor rooms: the gate again, plus the Rafflesia's
 *                       four. Fountain Square and the Outside Catacombs.
 *     SND_BANK_ASAG   - Asag's arena. TWO clips: the demon speech and the
 *                       explosion, both shared with
 *                       the boss bank because both bosses open with it.
 *
 *   The tag is a MASK, not a single value — an effect can be in more than one
 *   bank, at the cost of one copy in each. See the SoundBank enum below.
 *
 * WHY THE RABISU'S FIVE ARE ALL BANKED, AND WHY THREE OF THEM USED TO BE NOT
 *   FIREBALL, BOOM and EXPLODE fire DURING the fight, and a bank swap is a CD
 *   read — it cannot happen mid-room. That is why they were made RESIDENT: at
 *   the time it was the only way to guarantee they would be loaded when the
 *   fight ran. It was 47 KB, and A RESIDENT CLIP IS CHARGED TWICE — once
 *   against the permanent block, and again against the region, which is
 *   whatever is left after it.
 *
 *   >>> IT WAS NEVER NECESSARY, AND ASAG'S ARENA IS WHAT MADE IT WORTH FIXING.
 *   <<< The three are played by src/rabisu.c and src/rabisu_boss.c and by
 *   nothing else, world.c places a Rabisu in the Garden Courtyard and nowhere
 *   else, and the BOSS bank is loaded for the whole of that room — cutscene and
 *   fight alike. So the guarantee the residency was buying was already being
 *   bought by the bank. Moving all three onto SND_BANK_BOSS took bank_base from
 *   0x51C10 to 0x46150 and the region from 185 KB to 232 KB, which is 47 KB
 *   handed to EVERY bank at once. `spare' went from 12.2 KB to 45.8 KB.
 *
 *   THE SAME MISTAKE IS THE EASIEST ONE TO MAKE WITH ASAG. Its arena is a
 *   one-way pocket with a bank of its own that is loaded for the entire room, so
 *   an effect that plays mid-fight belongs in SND_BANK_ASAG, not in the
 *   residents. See the note on that enum value.
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
 *   re-read from \SND\ on the transition. 182 KB of resident RAM copies is not
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
    /* >>> THE THREE HARDWARE-LOOPED CLIPS POISON THEIR VOICES FOR BORROWING.
       <<< This one, SFX_TNTCL_WRTH above and SFX_WATER below are the only
       samples in the game with the ADPCM loop-start flag (0x04) on block 0, and
       the SPU latches that block's address into the voice's REPEAT-ADDRESS
       register when it decodes it. Nothing ever writes that register back —
       sound_play() sets the START address and PSn00bSDK does not even define
       the repeat one — so voices 17, 18 and 19 keep a stale repeat address for
       the rest of the run, and any ONE-SHOT later given one of those voices
       runs off its end into whatever the current bank has at that address.

       It presents as silence, or as a clip that plays only part of itself, with
       no error. Asag's laser and vomit were put on 18 and 17 and did exactly
       that. Borrow 13..15, 20 or 22 instead; full account and the one-line test
       for "is this clip hardware-looped" are in STEP 6 of
       tools/ADDING_A_SOUND.txt. */
    SFX_SPDR_WLK   = 19, /* spider scuttle, HARDWARE-looped while any spider walks */
    SFX_SPIT       = 20, /* spider fires a web (one-shot, once per web)            */
    SFX_MCHNE      = 21, /* grinding machinery — the piano room's sinking bookcase.
                            2.80 s at 11025 Hz; piano_props.c plays it twice back
                            to back and times the descent to match. */
    /* ---- The Rabisu. ALL FIVE are SND_BANK_BOSS; three of them used to be
       resident and did not need to be. See the bank note above. ---- */
    SFX_FIREBALL   = 22, /* BANKED (boss). A fireball leaves the boss's chest
                            (1.1 s)                                             */
    SFX_BOOM       = 23, /* BANKED (boss). One poly of the light-beam path erupting (1.1 s).
                            Retriggered every RBS_BEAM_STEP (0.3 s) as the beam
                            walks, so each one is cut short by the next and the
                            attack reads as a chain of detonations — only the
                            last plays its tail out. */
    SFX_EXPLODE    = 24, /* BANKED in BOTH boss banks. The death lights come up
                            and the body starts coming apart. 5.374 s, and BOTH
                            bosses cut their burn plus fade to exactly that, so
                            the lights are lit for the length of this clip and
                            no part of it is left hanging: RBE_T_D_BURN +
                            RBE_T_D_FADE for the Rabisu, ABE_T_D_BURN +
                            ABE_T_D_FADE for Asag. Retrim the clip and BOTH
                            pairs have to move.                                 */
    SFX_EMERGE     = 25, /* BANKED. Light being hauled up out of the ground.
                            11.1 s, which covers the reveal's 3 s of lights plus
                            its 5 s rise. Also the light beam's charge tell,
                            where it is deliberately cut after ~1.5 s by the
                            first poly igniting.                                */
    SFX_DMNSPEAK   = 26, /* BANKED, in BOTH boss banks. One line of scripture;
                            played once per line, the second retriggering over
                            the first. Two bosses now open with it — the
                            Rabisu's reveal and Asag's — and they sit in
                            different banks, so it is tagged BOSS|ASAG and
                            carries a copy in each. It is the whole of
                            SND_BANK_ASAG, which was empty; see sound.c.        */
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
       resident headroom against 13.3 KB. (That left ~6.7 KB spare at the time;
       SFX_RUMBLE's house copy has since taken most of it, so `spare` is 3.3 KB
       — check it rather than quoting this line.) A menu blip is
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
       headroom was 6.6 KB (3.3 KB now), so it cannot be resident; the house
       bank has the same figure spare, so it cannot go there either. It joins the
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
    /* The Mushroom Head's scream — the wet hiss its cap makes when it splits
       open. 2.00 s at 11025 Hz, 12.3 KB. GARDEN bank, and GARDEN ONLY: the
       house bank is the largest of the four and so sets `spare`, which is now
       3.3 KB — this clip does not fit it, and could not be made resident for
       the same reason. A mushroom placed inside the house would therefore be
       silent; the garden is where it lives. (SFX_RUMBLE below IS in both banks
       now, but only because re-cutting dogdie paid for it — that headroom is
       spent, so this one still cannot follow.) See tools/ADDING_A_SOUND.txt. */
    SFX_HISS       = 35,
    /* The Living Statue: stone grinding on stone. It plays on the frame the
       statue TELEPORTS, and again on the frame it is destroyed — those are the
       only two noises it makes (it is also the one enemy that is silent while
       it stalks, which is the point of it). 2.12 s at 11025 Hz, 13.0 KB.
       It is ALSO Hadad's arrival and death cue (hadad.h), reused deliberately
       because it is the noise stone makes moving and he is stone.

       HOUSE | GARDEN — one copy in each, 13.0 KB apiece. It was GARDEN ONLY
       while Hadad only ever stood on the Rear Gate's plinth; the moment he
       could be placed in the West Corridor, Reception or the Library it had to
       be in the house bank too, or he would have arrived in silence in exactly
       those rooms and nothing would have said so. It did NOT fit: house is the
       largest bank and so sets `spare`, which was 6.6 KB. dogdie.vag was at
       22050 Hz with no reason on record, against the 11025 house standard, and
       re-cutting it freed 9.8 KB. That is what bought this. `spare` is now
       3.3 KB — SMALLER THAN THIS CLIP, so the same trick cannot be repeated
       without finding more room. */
    SFX_RUMBLE     = 36,
    /* The Rear Gate's two grinders driving along their rails, played three
       times back to back on each throw of the corridor lever. 1.76 s at
       11025 Hz, 10.9 KB, and grinder_puzzle.c's GP_TRAVEL_FRAMES is cut to
       exactly three of them — retrim this clip and that constant has to move
       with it, the same contract SFX_GATE has with door_anim.c.

       GARDEN bank, and GARDEN ONLY. Made resident it would push bank_base up by
       its own 10.9 KB and overflow the HOUSE bank — the house bank is the
       largest, so it both sets `spare` (3.3 KB now, which this does not fit) and
       has no room to be squeezed. The garden bank runs to 127 KB of the region's
       185, so a copy there is free and `spare` does not move. A grinder placed
       inside the house would be silent. */
    SFX_GRIND      = 37,
    /* ---- SFX_RUMBLE on four more voices. ALIASES, in the SFX_RBS_SWING sense:
       NULL in sfx_files[], no second copy in SPU RAM, no disc entry — just four
       more slots pointing at the one upload so four more voices can play it.

       They exist for the quake (src/quake.c), which fires the rumble SIX times
       at 30-frame spacing across a 3 s shake. The clip is 127 frames long, so up
       to five are sounding at once and on ONE voice each trigger would simply
       cut the last — a stutter, not an earthquake. Five voices is exactly enough
       for six plays: by the time #6 starts at frame 150, #1 (frames 0..127) has
       finished, so the round-robin in quake.c never lands on a voice still in
       use.

       >>> THESE ARE BORROWED VOICES, AND THAT IS ONLY SAFE IN THE HOUSE. <<<
       The SPU's 24 voices are all spoken for (see sfx_channel), so rather than
       take one from anything that can sound in the rooms that quake these sit on
       top of four that CANNOT: GAS, PULL and HISS are GARDEN-bank monsters and
       EMERGE is a BOSS-bank one. Both quake sites — the Attic Exit's exit door
       and the East Hall's arrival from the wrecked Library — are HOUSE rooms, so
       those four voices are guaranteed idle every time one runs. Playing a quake
       in a garden or courtyard room would cut a flower, a mushroom or the boss's
       charge tell — pick different voices before moving it.

       SFX_RUMBLE itself is BANKED (house|garden), so unlike RBS_SWING these
       cannot be copied once at startup: their source address moves with every
       bank load. load_bank re-copies them on its way out. */
    SFX_RUMBLE_2   = 38,
    SFX_RUMBLE_3   = 39,
    SFX_RUMBLE_4   = 40,
    SFX_RUMBLE_5   = 41,
    /* ---- The Hadad Death Scene's two clips (src/hadad_grinder.c) -----------
       Both GARDEN, and garden only: the scene happens in the Rear Gate and
       nowhere else, and the Rear Gate is the one room in the game that can play
       them. Made resident they would be charged twice over — bank_base up by
       their own 49.5 KB AND the region down by it — which puts the 186 KB house
       bank far outside a 140 KB region. In the garden bank they are free in the
       sense that matters: `spare` is sized by the LARGEST bank, and garden goes
       130 KB -> 180 KB against house's 186 KB, so it is still not the largest
       and `spare` does not move off 3.3 KB. That leaves 6 KB of garden headroom
       before the two banks swap places and `spare` starts shrinking — re-run
       the STEP 3 arithmetic in tools/ADDING_A_SOUND.txt before spending it.

       >>> hadad_die's LENGTH IS LOAD-BEARING. <<< 3.45 s = 207 frames, and
       hadad_grinder.c's HG_T_ROAR is cut to it so the grey burst lands on the
       last frame of the roar. Retrim the clip and that constant must move with
       it — the same contract SFX_GRIND has with GP_TRAVEL_FRAMES.

       spirit_woosh is 4.49 s and is DELIBERATELY longer than the 3 s climb it
       covers: it is still sounding as the camera turns back to the player. */
    SFX_HAD_DIE    = 42,  /* BANKED (garden). Hadad's death roar, 3.45 s      */
    SFX_WOOSH      = 43,  /* BANKED (garden). The spirit flying away, 4.49 s  */
    /* >>> A SECOND COPY OF SFX_MCHNE, RE-CUT TO FIT THE GARDEN BANK. <<< The
       Greenhouse's vine curtain winds up to the same grinding machinery the
       piano-room bookcase and the Attic Exit's cage gate use, but SFX_MCHNE is
       HOUSE-only and 17.3 KB, and the garden bank had 9.2 KB of headroom left.
       A two-bank tag on the existing effect would have overflowed the region by
       8 KB — and an overflow is SILENT (load_vag_at drops the clip and it is
       mute forever), which is why the STEP 3 arithmetic in
       tools/ADDING_A_SOUND.txt comes before the code.

       So this is the same recording at 8000 Hz and trimmed to 1.8 s: 8.1 KB,
       which fits. It is a SEPARATE EFFECT rather than a re-cut of the shared
       one because SFX_MCHNE's 2.8 s length is load-bearing at both its existing
       call sites — chainlink_door.c and piano_props.c each fire it twice back
       to back and time their travel to it — and the runbook's rule is that a
       shared clip is shared.

       >>> ITS LENGTH IS LOAD-BEARING TOO. <<< GHB_RAISE_FRAMES in
       src/greenhouse_puzzle.c is cut to these 1.8 s (108 frames) so the grind
       covers the whole travel and stops with it, the same contract SFX_GRIND
       has with GP_TRAVEL_FRAMES.

       >>> AND IT MAKES GARDEN THE LARGEST BANK. <<< `spare` was 3.3 KB and is
       now 1.2 KB, and it is the GARDEN bank that sets it from here rather than
       HOUSE. Re-run the STEP 3 arithmetic before adding anything at all.
       (That 1.2 KB is history — SFX_WATER below paid the bank back; the numbers
       to trust are the ones in its block, not this one.) */
    SFX_MCHNE_GH   = 44,  /* BANKED (garden). SFX_MCHNE re-cut, 1.8 s          */
    /* RUNNING WATER, and the only HARDWARE-LOOPED effect outside the monsters.
       The Valve Puzzle's Maze One pipe opens a drain and this is what the drain
       sounds like, in Maze One, Fountain Square and the Rear Gate, from the
       moment that pipe is turned until the game is reset. Started and stopped
       by valve_puzzle_area_sound() on every room entry — see src/valve_puzzle.c.

       >>> IT LOOPS IN THE SPU, NOT IN C. <<< water.vag was encoded with
       wav_to_vag.py's --loop, which marks the first ADPCM block 0x06 and the
       last 0x03, so the voice repeats the sample forever once keyed on and one
       sound_play() is the whole of "start it". sound_stop() is therefore
       MANDATORY on the way out of those three rooms: nothing else ever ends it,
       and a loop left running would go on reading whatever the next bank load
       puts at that address (the note above load_bank in sound.c is about
       exactly this trap, which the spider scuttle found first).

       BANKED (garden). All three rooms it plays in are on SND_BANK_GARDEN —
       check main.c's sound_bank_select if a fourth is ever added, because a
       banked loop is silent, not broken, in a room whose bank lacks it.

       >>> IT COST TWO OTHER CLIPS THEIR TOP OCTAVE. <<< At 1.98 s / 11025 Hz it
       is 12.5 KB and the garden bank had 1.2 KB free. spdr_wlk.vag and
       tntcl_die.vag were the last two clips still at 22050 Hz for no recorded
       reason; re-cutting both to the house 11025 freed 23.5 KB — from HOUSE as
       well as GARDEN, since both are in both banks. After it:

           bank_base 0x51C10   region 189424
           house 162560   boss 142528   garden 177216   intro 22080
           spare 12208

       so GARDEN is the largest bank still, but with real headroom for the first
       time since the Hadad death scene. Nothing else is left off the standard —
       the next clip that does not fit has to be trimmed or banked, not
       re-sampled. */
    SFX_WATER      = 45,  /* BANKED (garden). HARDWARE-LOOPED, 1.98 s          */

    /* ---- ASAG'S THREE ATTACKS. ALL BANKED (asag), and that is forced ------
       SND_BANK_ASAG is the only bank his arena loads, so an effect that plays
       DURING his fight has nowhere else to be: resident costs permanent RAM in
       all twenty-seven rooms and a clip in any other bank is silent here. This
       is the rule STEP 3 of tools/ADDING_A_SOUND.txt draws out of the Rabisu's
       fireball and boom, applied to the second boss the first time it had a
       fight to apply it to.

       THEY WERE FREE. The three take the asag bank 87,872 -> 158,400 of the
       region's 237,232, and BOSS is still the largest at 190,336, so `spare`
       stayed at 46,896. Filling a bank that is not the largest costs nothing,
       which is the whole point of the note above SFX_DMNSPEAK.

       >>> AND THE BOULDERS BORROW SFX_RUMBLE, which is why that one is now
       HOUSE|GARDEN|ASAG. <<< A third copy at the asag bank's own address; a
       two-bank tag is a mask and costs its own bytes each time (STEP 5). It was
       already the right sound for rock hitting a floor and the alternative was
       a fourth clip saying the same thing. */
    SFX_VOMIT      = 46,  /* BANKED (asag). Asag starts spewing. 2.21 s        */
    SFX_SLAM_ASAG  = 47,  /* BANKED (asag). His head hits the floor. 2.40 s    */
    SFX_LASER      = 48,  /* BANKED (asag). The beam leaves him. 4.44 s, which
                             covers the 3 s sweep and its tail; the clip is
                             longer than the beam on purpose, so it does not cut
                             out while the floor is still burning.             */
    /* ---- THE CATACOMBS ----------------------------------------------------
       CHAPTER 3's first sound, and as of now the whole of SND_BANK_CATACOMBS.
       Oil leaving the dispenser in the Catacombs Entry's burial hall and going
       into the Helluminator — see src/oil_dispenser.c, which is the only thing
       that fires it and fires it only on a pour that actually moved oil.

       IT WAS FREE, and the note above SND_BANK_CATACOMBS predicted exactly this:
       4160 SPU bytes into a bank that was empty, in a region whose ceiling BOSS
       sets at 190,336, so `spare` did not move off 46,896. That bank has about
       186 KB before it starts costing the other five anything.

       OFF THE POOL, ON VOICE 13 — see the note in sound.c. */
    SFX_GLUG       = 49,  /* BANKED (catacombs). Oil pouring. 0.65 s           */
    /* ---- THE CATACOMB DOOR ------------------------------------------------
       CHAPTER 3's second sound, and the door sound for every transition INSIDE
       the chapter - starting with the Catacombs Entry's burial hall <-> the Up
       Down Maze. It is fired by src/door_anim.c's DOOR_PANEL_CATACOMB variant
       and by nothing else, the same one-caller contract SFX_GATE has with
       DOOR_PANEL_GATE.

       BANKED (catacombs), AND THAT IS THE EASY HALF OF THE ARGUMENT: a door
       sound plays during the transition, i.e. BEFORE main.c's STATE_LOADING has
       swapped any bank, so what matters is the bank the room the player is
       LEAVING had loaded. Both ends of every transition this plays on are inside
       Chapter 3, so that bank is SND_BANK_CATACOMBS either way. Compare
       SFX_GATE, which had to be in TWO banks precisely because it plays on the
       way out of rooms on both sides of a bank boundary.

       IT WAS FREE, like the glug before it. 34,240 SPU bytes into a bank that
       held 4,160, in a region whose ceiling BOSS sets at 190,336; the catacombs
       bank is now 38,400 and `spare` did not move off 46,896. That bank still
       has about 150 KB before it costs any other bank anything.

       5.43 s, WHICH IS LONGER THAN THE TRANSITION IT PLAYS UNDER (4.0 s, see
       CAT_TOTAL_FRAMES in src/door_anim.c) - deliberately, and the same way
       round as SFX_DOOR's 5.07 s under the shared clock's 5.0 s. The tail runs
       on under the loading screen rather than being cut off at the fade, which
       is what stops the black screen being silent.

       >>> ITS LENGTH IS NOT LOAD-BEARING THE WAY SFX_GATE'S IS. <<< That clip's
       length sets GATE_SWING_FRAMES, so retrimming it desynchronises the leaf
       from the creak. This one's swing is set by the BRIEF (3 s) and not by the
       clip, so trimming it only shortens what is heard.

       OFF THE POOL, ON VOICE 14 - see the note in sound.c. */
    SFX_CTCMBDR    = 50,  /* BANKED (catacombs). The catacomb door. 5.43 s     */
    /* ---- THE CRAWLER ------------------------------------------------------
       CHAPTER 3's first MONSTER, and the first thing to put a third and fourth
       clip in SND_BANK_CATACOMBS. Both are fired by src/crawler.c and nothing
       else. It has a third sound and it is NOT here: the crawler walks on
       SFX_SPDR_WLK, which already exists, and that entry simply gained
       SND_BANK_CATACOMBS in sfx_bank[] — a third copy of an 11.4 KB sample
       against a bank with ~150 KB spare, which is far cheaper than a fourth
       clip and is why the spider's scuttle is deliberately reused rather than
       re-recorded. Say so at the call site too; see crawler.c.

       STILL FREE, on the arithmetic the glug and the door both recorded:
       the catacombs bank goes 38,400 -> 73,664, and the region's ceiling is set
       by BOSS at 190,336, so `spare` does not move off 46,896. That bank has
       about 116 KB left before it costs any other bank anything.

       OFF THE POOL, ON VOICES 15 AND 9 - see the note in sound.c. Voices 13 and
       14, the usual "free block" this chapter has been drawing on, are NOT
       available: SFX_GLUG and SFX_CTCMBDR are already on them and both are
       SND_BANK_CATACOMBS, so a crawler could key one on over a pour or a door. */
    SFX_CRWL_SCRM  = 51,  /* BANKED (catacombs). The crawler's scream: on
                             waking, at the top of each fresh rush after a
                             retreat, and on death. 1.90 s                     */
    SFX_CRWL_WHSP  = 52,  /* BANKED (catacombs). The idle whisper, re-triggered
                             on an interval while the player stands inside an
                             idle crawler's listening radius. 1.82 s           */
    /* ---- THE LUMBERER ----------------------------------------------------
       Chapter 3's second MONSTER (src/lumberer.c), and the first enemy in it to
       get a voice of its own rather than borrowing the crawler's. Both clips
       are SND_BANK_CATACOMBS and nothing outside src/lumberer.c plays either.

       FREE, on the arithmetic of STEP 3: the catacombs bank goes 96,192 ->
       126,400 and the region's ceiling is still BOSS at 190,336, so `spare`
       stays at 46,896. That bank has about 111 KB left before it costs any
       other bank anything.

       ON VOICES 16 AND 21, BOTH BORROWED, both on the eviction argument the
       glug and the door spell out in sound.c: SFX_ZOMBIE is SND_BANK_HOUSE and
       SFX_EXPLODE is SND_BANK_BOSS|SND_BANK_ASAG, none of those three banks can
       be loaded while SND_BANK_CATACOMBS is, and neither clip is resident. The
       chapter's own voices - 13 the glug, 14 the door, 15 and 9 the crawler's
       two - were all spoken for, which is why this pair had to go further out.
       NEITHER 16 NOR 21 IS POISONED: checked with STEP 6's script rather than
       assumed, zombie_2.vag carries its loop flag on block 1181 of 1182 and
       explode.vag on 2115 of 2116, so both are ordinary one-shots that have
       never moved a repeat address. (17, 18 and 19 remain untouchable, and note
       18 is in this very bank - it is the crawler's scuttle.) */
    SFX_LMBR_MOAN  = 53,  /* BANKED (catacombs). The walking moan, re-triggered
                             from C on an interval while the body TRAVELS, the
                             way zombie.c re-triggers SFX_ZOMBIE - it is not a
                             hardware loop and must not become one, or it would
                             poison voice 16 for everyone. 2.96 s              */
    SFX_LMBR_YELL  = 54,  /* BANKED (catacombs). The shockwave going out, on the
                             first frame of the strike. Replaces the borrowed
                             SFX_CRWL_SCRM at that call site. 1.82 s           */
    /* ---- THE CRIB'S LOOP, AND IT IS THE CHAPTER'S SIXTH BORROWED VOICE.
       The glug has 13, the door 14, the crawler's scream 15 and its whisper 9,
       the Lumberer's moan 16 and its yell 21 - all six SND_BANK_CATACOMBS, so
       none of them can be borrowed again. A cot pouring Creeps while a crawler
       screams, or while the player pours oil in the room behind, are both
       ordinary moments in this chapter.

       SO IT TAKES 20, one voice further out on exactly the argument the moan
       and the yell use:
         20  SFX_DMNSPEAK (BOSS | ASAG) and SFX_HAD_DIE (GARDEN)
       Neither is resident, and none of BOSS, ASAG or GARDEN can be loaded while
       SND_BANK_CATACOMBS is - the catacomb mouth is a one-way door - so neither
       can sound down here at all.

       AND THE POOL WAS NEVER AN OPTION, on the rule rather than a judgement:
       this clip is re-keyed for the WHOLE of a thirty-second-plus encounter, and
       its raw slot would be FIRST_VOICE + (55 % 8) = 8 - which is outside the
       1..8 pool's own arithmetic only by luck, and every other id in the pool is
       a weapon or a footstep the player is firing continuously while fighting
       ten Creeps. A looped cue must be alone (STEP 6).

       20 IS NOT POISONED, checked with STEP 6's script rather than assumed:
       dmnspeak.vag carries its loop flag on block 3375 of 3376 and hadad_die.vag
       on 1358 of 1359, so both are ordinary one-shots and neither has ever moved
       that voice's repeat address.

       >>> AND THIS CLIP MUST STAY A C-SIDE RETRIGGER, for the reason the
       Lumberer's moan must. <<< crib.c re-keys it every CRIB_LOOP_FRAMES while
       the cot is in CRIB_ACTIVE, the way zombie.c re-keys SFX_ZOMBIE. Giving it
       a hardware loop instead would put 0x04 on its block 0 and poison voice 20
       for good - which is what happened to 17, 18 and 19 and cost a session to
       find. Verified a one-shot on the same test: block 2301 of 2302.

       THE BANK WAS FREE. 36,864 bytes takes SND_BANK_CATACOMBS to 163,264
       against the 190,336 BOSS sets `spare` by, so it cost no other bank a byte
       and `spare` stayed on 46,896. About 27 KB left in the chapter before this
       becomes the largest bank; re-run STEP 3 before spending it. */
    SFX_CREEP      = 55,  /* BANKED (catacombs). The crib's encounter loop: keyed
                             on when a swing wakes the cot and re-keyed every
                             CRIB_LOOP_FRAMES until all ten Creeps are dead.
                             5.85 s, which is where CRIB_LOOP_FRAMES comes from */
    SFX_COUNT      = 56,
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
    /* ASAG'S ARENA, and NOTHING ELSE IS ON IT. It started EMPTY — and that
       empty bank was the point of it: the room
       is reached only by a one-way drop (src/asag_arena.h), so no monster in the
       game can be heard down there and not one shared effect has to be carried.
       The whole 232 KB bank region is Asag's, which is by a wide margin the
       largest sound budget any room in this game has ever had.

       WHAT PLAYS DOWN THERE WITHOUT BEING IN IT: everything the player brings.
       SWING, AXEHIT, GR_SHOT, GR_RELOAD, HURT, DIE, STEP1/STEP2, PICKUP, DOOR,
       UNLOCK, SLAM and the three menu blips are all RESIDENT and stay put under
       every bank. That is the list the arena was designed around and it is
       already complete — nothing needs adding to this bank to make the player
       work.

       >>> ANYTHING THE FIGHT PLAYS MUST BE IN HERE OR RESIDENT, AND HERE IS THE
       RIGHT ANSWER. <<< A bank swap is a CD read and cannot happen inside a
       room, so an effect in another bank is simply silent down there — the
       Rabisu's FIREBALL/BOOM/EXPLODE were made RESIDENT for exactly that reason
       and it cost the whole game 47 KB of permanent SPU RAM. It does not have to
       cost that again: the arena's bank is loaded for the WHOLE of the room,
       cutscene and fight alike, and the fight cannot happen anywhere else. Put
       Asag's clips HERE, not in the residents.

       >>> IT IS NO LONGER EMPTY: SFX_DMNSPEAK AND SFX_EXPLODE ARE IN IT. <<<
       Asag's opening puts one line of the demon speech over each of its two
       subtitles the way the Rabisu's reveal does, and his DEATH is the Rabisu's
       death with the same explosion clip under it. Both are tagged BOSS|ASAG,
       so a copy of each sits in both banks.

       >>> AND THE FIGHT HAS NOW SPENT SOME OF IT, EXACTLY AS THE PARAGRAPH
       ABOVE SAID TO. <<< SFX_VOMIT, SFX_SLAM_ASAG and SFX_LASER are Asag's own
       three attack sounds and SFX_RUMBLE is tagged ASAG for the boulders, so
       the bank now holds six clips:

           dmnspeak  explode  rumble  vomit  slam_asag  laser

       158,400 of the region's 237,232, and BOSS still sets `spare` at 190,336 —
       so all four cost nothing. There are ~32 KB left before this bank becomes
       the largest and starts eating `spare` for real.

       Re-run STEP 3 of tools/ADDING_A_SOUND.txt before spending more, and re-do
       the arena door's HEAP peak with it (PART 6
       of tools/ADDING_THE_ASAG_FIGHT.txt) — the SPU is not the binding
       constraint down there and the heap is. */
    SND_BANK_ASAG  = 16,
    /* THE CATACOMBS - CHAPTER 3, AND IT IS EMPTY, exactly as SND_BANK_ASAG was
       on the day the arena landed and for a stronger version of the same
       reason. The catacomb mouth is a ONE-WAY door: nothing in the mansion or
       the garden can be walked back to, so no monster in the game today can be
       heard down there and none of their clips has to be carried in. Everything
       the PLAYER makes a noise with - the axe, the gun, the lantern, the
       footsteps, the hurt and the death, the menu blips - is SND_RESIDENT and
       is unaffected by which bank is in.

       So the whole shared region, about 232 KB, belongs to Chapter 3, and every
       clip it gains should be tagged SND_BANK_CATACOMBS and nothing else. The
       ceiling to watch is the point at which this becomes the LARGEST bank:
       `spare` is the region less the largest, BOSS sets it today at 190 KB, and
       a catacombs bank has that much before it starts costing the other four
       anything. Re-run STEP 3 of tools/ADDING_A_SOUND.txt before spending it.

       >>> AND UNLIKE EVERY BANK ABOVE, THIS ONE IS NOT ONLY AN SPU DECISION.
       <<< The transition that loads it also hands back the mansion's and the
       garden's texture RAM (src/area_bank.h). The two halves are deliberately
       separate - main.c's sound_bank_select line does this one, and
       chapter_enter_catacombs() does the other - because they are needed at
       different points in STATE_LOADING's ordering. */
    SND_BANK_CATACOMBS = 32,
} SoundBank;

void sound_init(void);

/* Bring the SPU up and upload SFX_GR_SHOT alone, so the boot splash's yellow
   flash has a gunshot to fire (src/splash.h). Call after CdInit() and before
   splash_prelude(); sound_init() below still runs in its usual place at the end
   of the startup block and re-lays the sample properly. See the note on the
   definition for why the double upload is the right trade. */
void sound_splash_init(void);

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
