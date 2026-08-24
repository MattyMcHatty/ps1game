#include <stdint.h>
#include <psxcd.h>
#include <psxspu.h>
#include "cdaudio.h"

/* CD-DA looping strategy
 * ----------------------
 * Two emulator-portability problems had to be solved:
 *
 * 1) PLAYBACK: CdlPlay with a track-number parameter is "not supported by some
 *    emulators" (per the SDK docs — DuckStation ignores it and plays nothing).
 *    Fix: resolve the track's start location from the disc TOC and seek there
 *    with CdlSetloc, then CdlPlay with NO parameter (works everywhere).
 *
 * 2) LOOPING: the auto-pause / CdlDataEnd interrupt is implemented
 *    inconsistently across emulators, so relying on it to detect track end
 *    never fired reliably. Instead we poll the physical playback position
 *    (CdlGetlocP) and the drive status, and watch several independent
 *    end-of-track signals (see cdaudio_update). The most robust one is the
 *    known END SECTOR of the track (the disc lead-out, queried via CdlGetTD):
 *    when the play position reaches it we re-seek and replay. This works even
 *    on emulators (PCSX-Redux) whose drive model keeps reporting "playing" with
 *    an advancing position after the audio output has actually stopped — none
 *    of the stop-based signals fire there, but the position still crosses the
 *    track end.
 */

#define CD_POLL_INTERVAL   20   /* frames between position polls (~0.33s @60fps) */
#define CD_GRACE_POLLS     10   /* polls to skip after (re)starting — covers seek+spinup */

/* THE LAST SECOND OF THE TRACK IS MUSIC AND MUST BE HEARD.
 * ---------------------------------------------------------
 * Signal 3 below restarts the track when the play position comes within
 * CD_END_MARGIN sectors of the end, and the audio stops dead the instant that
 * fires — so the margin is not a safety cushion, it is a piece cut off the end
 * of every repeat. At 75 sectors (a full second) Hadad's twenty-second stalk
 * looped a second early, every time, which is audible as a phrase that never
 * finishes.
 *
 * The margin cannot simply go to zero: the position is only sampled every
 * CD_POLL_INTERVAL frames, and one slow poll is 25 sectors of travel, so a
 * small margin with a coarse poll would overshoot into the lead-out instead.
 * The fix is to make the SAMPLING fine where it matters rather than to keep
 * throwing away music: within CD_END_APPROACH of the end the poll rate goes up
 * to CD_END_POLL_INTERVAL, which is 2.5 sectors of travel, and the margin then
 * only has to cover that. Eight sectors is three of those fast polls, about a
 * ninth of a second — under the CD's own frame-accurate seek and far below what
 * can be heard as a truncation.
 *
 * The fast window is bounded (a couple of seconds an iteration, and only when
 * the end sector is known) so the extra CdControlB traffic never runs over the
 * body of a track. */
#define CD_END_POLL_INTERVAL 2  /* frames between polls near the end of a track */
#define CD_END_APPROACH   150   /* start polling fast this many sectors (~2s) out */
#define CD_END_MARGIN       8   /* restart this many sectors (~0.1s) before the end */

/* The three "it stopped" signals are stated in FRAMES, not in polls, so they
   mean the same length of time whichever rate is running. Dividing by the live
   interval reproduces the old 2/2/4 exactly at CD_POLL_INTERVAL — and stops a
   fast poll near the end from firing them eight times sooner, which would cut
   the tail off again from the other direction. */
#define CD_STALL_FRAMES    40   /* non-advancing position for this long = ended */
#define CD_NOTPLAY_FRAMES  40   /* "not playing" for this long = ended          */
#define CD_FAIL_FRAMES     80   /* position reads failing this long = stopped   */

/* Per-track LOOP OFFSET
 * ---------------------
 * A repeat does not have to begin where the track begins. CD-DA runs at exactly
 * 75 sectors a second, so an offset in seconds is an exact sector count and the
 * seek lands on a frame boundary with no drift however many times it loops.
 *
 * Only Hadad's track uses one: its first four seconds are a one-off swell that
 * belongs to his arrival, and hearing it again every twenty seconds would turn
 * the cue into a metronome. The FIRST play still starts at the top — that is the
 * whole point of the swell — so the offset is applied by the RESTART path
 * (issue_replay) and not by cdaudio_play.
 *
 * 4.015 s x 75 = 301.125 sectors; 301 is the frame the loop starts on. Expressed
 * as the numerator over 1000 rather than as a float because there is no FPU here
 * and the intent is easier to check against the design than a bare 301 would be. */
#define CD_SECTORS_PER_SEC   75
#define CDAUDIO_STALKER_LOOP_MS  4015   /* 4.015 s -> 301 sectors */

/* Per-track mix level, applied by cdaudio_play().
 * ------------------------------------------------
 * CD audio passes through TWO independent volume stages on the way out, and
 * this matters because attenuating both multiplies them together:
 *
 *   1. the CD-ROM CONTROLLER's mixing matrix (CdMix / CdlATV), 0-255 with 128
 *      meaning 100%. This is the drive's own output level, applied before the
 *      audio ever reaches the SPU.
 *   2. the SPU's CD input volume (SpuSetCommonCDVolume), 0-0x7FFF.
 *
 * >>> THE ATTENUATION IS AT STAGE 1, AND STAGE 2 STAYS AT FULL. <<<
 * Setting the SPU register alone was tried first and changed nothing audible,
 * so that stage is not the one governing playback here — which is consistent
 * with the CD-DA routing note in cdaudio_init() below: the SPU's CD path can
 * be bypassed, and where it is, its volume register goes with it. The drive's
 * mixer is upstream of all of that. Both stages exist on real hardware, and
 * 20% at either one is 20% out, so attenuating the reliable one is not a
 * workaround — it is just the stage that is always in circuit.
 *
 * Do NOT also drop CD_VOL_FULL to "help": 20% x 20% is 4%.
 *
 * The Garden Courtyard's track is the only one that plays over a fight rather
 * than over exploration, and at full level it buries the Rabisu's own sounds —
 * the light-beam detonations and the foot-slash wind-up especially, which the
 * player has to hear to time a parry off.
 *
 * This lives in cdaudio_play() rather than at the call site in rabisu_boss.c so
 * no caller has to remember to put the level back: every room starts its music
 * through here, so the next cdaudio_play always re-asserts the right level. */
#define CD_VOL_FULL    0x7FFF   /* SPU CD input: always full, see above  */
#define CD_MIX_FULL       128   /* drive mixer: 128 == 100%              */
#define CD_MIX_BOSS        26   /* 20% of CD_MIX_FULL                    */

static int           cd_audio_playing = 0;
static int           cd_track_num     = 0;
static int           cd_loop_mode     = 0;
/* Sectors past the track start that a REPEAT begins at. 0 for every track but
   Hadad's; see the loop-offset note above. */
static uint32_t      cd_loop_offset   = 0;

/* Resolved start location of the music track (from the TOC). */
static CdlLOC        cd_track_loc;
static int           cd_loc_valid     = 0;
/* Absolute sector at which the track ends (disc lead-out). 0 if unknown. */
static uint32_t      cd_end_sector    = 0;
/* Where playback was when cdaudio_suspend() halted the drive, so cdaudio_resume()
   can pick the track up mid-phrase. 0 means "start the track from the top". */
static uint32_t      cd_resume_sector = 0;

/* Position-polling state. */
static int           poll_tick        = 0;
/* Frames between polls RIGHT NOW: CD_POLL_INTERVAL for the body of a track,
   CD_END_POLL_INTERVAL once the position is inside CD_END_APPROACH of the end.
   See the end-margin note at the top. */
static int           poll_interval    = CD_POLL_INTERVAL;
static int           grace_polls      = 0;
static int           stall_count      = 0;   /* consecutive non-advancing reads */
static int           notplay_count    = 0;   /* consecutive "not playing" status reads */
static int           fail_count       = 0;   /* consecutive failed position reads */
static uint32_t      last_sector      = 0;

/* Returns 1 if the drive currently reports CD-DA playback, 0 if not, and -1 if
   the status could not be read. Uses a fresh CdlNop so the status is current
   (CdStatus() alone returns a stale cached value). */
static int drive_is_playing(void) {
    uint8_t st[8] = {0};
    if (!CdControlB(CdlNop, NULL, st)) return -1;
    return (st[0] & CdlStatPlay) ? 1 : 0;
}

/* Look up a track's start location. Tries CdGetToc first, then falls back to
   the CdlGetTD command. Must run on the main thread while the drive is idle. */
static int resolve_track_loc(int track, CdlLOC *out) {
    CdlLOC toc[100];
    int n = CdGetToc(toc);
    if (n > 0) {
        for (int i = 0; i < n; i++) {
            if (toc[i].track == track) { *out = toc[i]; return 1; }
        }
        if (track >= 1 && track - 1 < n) { *out = toc[track - 1]; return 1; }
    }

    /* Fallback: ask the drive directly for the track's start MSF. */
    uint8_t param  = itob(track);
    uint8_t res[8] = {0};
    if (CdControlB(CdlGetTD, &param, res)) {
        /* res[0] = status, res[1] = minute (BCD), res[2] = second (BCD) */
        out->minute = res[1];
        out->second = res[2];
        out->sector = 0;
        out->track  = (uint8_t)track;
        return 1;
    }
    return 0;
}

/* Convert a CdlLOC (absolute MSF, BCD) to an absolute sector count. */
static uint32_t loc_to_sector(const CdlLOC *l) {
    int m = btoi(l->minute);
    int s = btoi(l->second);
    int f = btoi(l->sector);
    return (uint32_t)(((m * 60) + s) * 75 + f);
}

/* Determine the absolute sector where the given track ends. For the last track
   on the disc this is the lead-out, obtained with CdlGetTD track 0 (the
   conventional lead-out query). Returns 0 if it cannot be determined.
   The returned value is in the same absolute-MSF basis as CdlGetlocP. */
static uint32_t resolve_track_end(int track) {
    uint32_t start = cd_loc_valid ? loc_to_sector(&cd_track_loc) : 0;

    /* The true end of a track is the start of the NEXT track; for the last
       track that is the lead-out. Query both and take the nearest boundary
       that lies after our track's start. */
    uint32_t best = 0;
    const uint8_t queries[2] = { itob(track + 1), itob(0) }; /* next track, then lead-out */
    for (int q = 0; q < 2; q++) {
        uint8_t param = queries[q];
        uint8_t res[8] = {0};
        if (!CdControlB(CdlGetTD, &param, res)) continue;
        /* res[0]=status, res[1]=minute(BCD), res[2]=second(BCD) */
        uint32_t sec = (uint32_t)(((btoi(res[1]) * 60) + btoi(res[2])) * 75);
        if (sec > start && (best == 0 || sec < best))
            best = sec;
    }
    return best;
}

/* Read current physical position. Returns 1 on success and fills *sector
   (absolute sector) and *track (decimal track number); returns 0 on error. */
static int read_position(uint32_t *sector, int *track) {
    uint8_t res[8] = {0};
    if (!CdControlB(CdlGetlocP, NULL, res)) return 0;
    CdlLOCINFOP *p = (CdlLOCINFOP *)res;
    int m = btoi(p->minute);
    int s = btoi(p->second);
    int f = btoi(p->sector);
    *sector = (uint32_t)(((m * 60) + s) * 75 + f);
    *track  = btoi(p->track);
    return 1;
}

/* Convert an absolute sector count back to a CdlLOC (absolute MSF, BCD) — the
   inverse of loc_to_sector, used to resume playback part-way through a track. */
static void sector_to_loc(uint32_t sec, CdlLOC *out) {
    out->minute = itob((uint8_t)(sec / (60 * 75)));
    out->second = itob((uint8_t)((sec / 75) % 60));
    out->sector = itob((uint8_t)(sec % 75));
    out->track  = 0;
}

/* Seek and begin CD-DA playback. `at` is an absolute sector to start from, or 0
   for the track's own start. CdlModeDA selects audio mode; speed bit stays clear
   (CD-DA must play at 1x). No CdlModeAP — we want continuous playback to end of
   disc and detect the end by polling. */
static void issue_play_from(uint32_t at) {
    uint8_t mode = CdlModeDA;
    CdControl(CdlSetmode, &mode, NULL);
    if (at) {
        CdlLOC loc;
        sector_to_loc(at, &loc);
        CdControl(CdlSetloc, &loc, NULL);
    } else if (cd_loc_valid) {
        CdControl(CdlSetloc, &cd_track_loc, NULL);
    }
    CdControl(CdlPlay, NULL, NULL);

    /* Reset poll state so the seek/spinup window isn't mistaken for a stall.
       The rate goes back to the slow one: whatever we just seeked to, we are no
       longer at the end of the track. */
    poll_tick     = 0;
    poll_interval = CD_POLL_INTERVAL;
    grace_polls   = CD_GRACE_POLLS;
    stall_count   = 0;
    notplay_count = 0;
    fail_count    = 0;
    last_sector   = 0;
}

/* Start the track from its beginning. */
static void issue_play(void) { issue_play_from(0); }

/* Start the track AGAIN — the loop restart. Identical to issue_play() for every
   track whose cd_loop_offset is 0, which is all of them but Hadad's; for that
   one it skips the arrival swell. Falls back to the track start if the location
   was never resolved, so a TOC read that failed is merely unmusical rather than
   a seek into the lead-out. */
static void issue_replay(void) {
    if (!cd_loop_offset || !cd_loc_valid) { issue_play(); return; }
    issue_play_from(loc_to_sector(&cd_track_loc) + cd_loop_offset);
}

void cdaudio_init(void) {
    /* Route CD audio into the SPU mixer at full volume, and leave it there for
       good: per-track levels are set on the DRIVE's mixer by cdaudio_play(),
       one stage upstream of this one. See the CD_MIX_* note above for why the
       attenuation is not done here... */
    SpuSetCommonCDVolume(CD_VOL_FULL, CD_VOL_FULL);
    /* ...and CRITICALLY, set bit 0 of SPUCNT (CD Audio Enable). Without this
       bit, accurate emulators (DuckStation) and real hardware will NOT mix CD
       audio into the output — only lenient emulators (PCSX-Redux) play it
       regardless. SpuSetCommonCDVolume only sets the volume registers, not this
       enable bit, so it must be set explicitly. Runs after sound_init()'s
       SpuInit(), so the SPU control register already exists. */
    SPU_CTRL |= 0x0001;

    cd_audio_playing = 0;
    cd_loop_mode     = 0;
    cd_loc_valid     = 0;
}

void cdaudio_play(int track, int loop) {
    cd_loc_valid  = resolve_track_loc(track, &cd_track_loc);
    cd_end_sector = resolve_track_end(track);

    /* Sanity check: only trust the end sector if the track is plausibly long
       (>= ~10s). A bogus short value would otherwise restart us near the track
       start in an infinite loop. If it fails, fall back to the other signals. */
    if (cd_loc_valid && cd_end_sector) {
        uint32_t start = loc_to_sector(&cd_track_loc);
        if (cd_end_sector < start + 750)
            cd_end_sector = 0;
    }

    /* Set the mix level for THIS track before it starts. Re-asserted on every
       play, which is what makes it self-correcting: leaving the courtyard for
       any room restores full volume without that room knowing anything about
       it. issue_play() on a loop restart does not re-issue this, so the level
       survives the track looping.

       The matrix is L-to-L and R-to-R only ({ v, 0, v, 0 }) — the SDK's stated
       default routing, with just the level changed. The cross terms are what
       downmix to mono, and setting them would collapse the stereo image as a
       side effect of turning the music down. */
    {
        uint8_t v = (track == CDAUDIO_COURTYARD_TRACK) ? CD_MIX_BOSS
                                                       : CD_MIX_FULL;
        CdlATV mix = { v, 0, v, 0 };
        CdMix(&mix);
    }

    /* Where a REPEAT of this track begins. Re-derived on every play so it can
       never be left set from a previous room's track — the same self-correcting
       arrangement the mix level above uses. */
    cd_loop_offset = (track == CDAUDIO_STALKER_TRACK)
                   ? (uint32_t)((CDAUDIO_STALKER_LOOP_MS * CD_SECTORS_PER_SEC) / 1000)
                   : 0;

    cd_audio_playing = 1;
    cd_track_num     = track;
    cd_loop_mode     = loop;
    issue_play();
}

void cdaudio_update(void) {
    if (!cd_audio_playing || !cd_loop_mode) return;

    if (++poll_tick < poll_interval) return;
    poll_tick = 0;

    /* Persistence, converted from frames to polls at whatever rate is running
       (see the CD_*_FRAMES note at the top). Never less than one poll. */
    int stall_limit   = CD_STALL_FRAMES   / poll_interval;
    int notplay_limit = CD_NOTPLAY_FRAMES / poll_interval;
    int fail_limit    = CD_FAIL_FRAMES    / poll_interval;
    if (stall_limit   < 1) stall_limit   = 1;
    if (notplay_limit < 1) notplay_limit = 1;
    if (fail_limit    < 1) fail_limit    = 1;

    /* Startup grace: ignore everything while the drive seeks and spins up.
       Prime last_sector so the first real comparison is meaningful. */
    if (grace_polls > 0) {
        grace_polls--;
        uint32_t s; int t;
        if (read_position(&s, &t)) last_sector = s;
        stall_count   = 0;
        notplay_count = 0;
        fail_count    = 0;
        return;
    }

    /* Different emulators expose end-of-track in different ways, so we watch
       several independent signals. All are checked only after the grace window,
       and each needs brief persistence, so none can false-trigger mid-track. */

    /* Signal 1: the drive status says it is no longer playing. This is the most
       direct indicator and is what catches PCSX-Redux. */
    int playing = drive_is_playing();
    if (playing == 0) {
        if (++notplay_count >= notplay_limit) { issue_replay(); return; }
    } else if (playing == 1) {
        notplay_count = 0;
    }

    /* Signals 2-5 come from the physical position. */
    uint32_t cur = 0;
    int      cur_track = 0;
    if (!read_position(&cur, &cur_track)) {
        /* Signal 2: position reads keep failing — drive has likely stopped. */
        if (++fail_count >= fail_limit) { issue_replay(); return; }
        return;
    }
    fail_count = 0;

    /* Signal 3: the play position has reached the end of the track (lead-out).
       This is the signal that works on emulators which keep "playing" with an
       advancing position past the audio end (PCSX-Redux). Only used when the
       end sector is known and the position is in the expected range (guards
       against a bogus reading restarting us mid-track). */
    if (cd_end_sector > CD_END_MARGIN &&
        cur >= (cd_end_sector - CD_END_MARGIN) &&
        cur <= (cd_end_sector + (75 * 5))) {
        issue_replay();
        return;
    }

    /* Signal 4: playback left our track (into the next track or the lead-out).
       cur_track == 0 is treated as "unknown" and ignored. */
    if (cur_track != 0 && cur_track != cd_track_num) {
        issue_replay();
        return;
    }

    /* Signal 5: position stopped advancing — drive reached end of disc. */
    if (cur <= last_sector) {
        if (++stall_count >= stall_limit) {
            issue_replay();
            return;
        }
    } else {
        stall_count = 0;
    }
    last_sector = cur;

    /* Close in on the end: once the position is within CD_END_APPROACH of the
       track end, poll fast so signal 3 can fire on a tight CD_END_MARGIN and the
       loop keeps the tail of the track. Only meaningful when the end sector is
       known — with cd_end_sector == 0 nothing here can measure "close". */
    poll_interval = (cd_end_sector > CD_END_APPROACH &&
                     cur >= (cd_end_sector - CD_END_APPROACH))
                  ? CD_END_POLL_INTERVAL : CD_POLL_INTERVAL;
}

/* Temporarily halt CD-DA so the drive is free for data reads (CdRead) mid-game.
   The original design loaded all assets at startup precisely to avoid competing
   with CD-DA; per-room geometry and sound-bank streaming need mid-game reads, so
   they must bracket them with suspend/resume or the drive hangs (reading data
   while audio streams). cd_audio_playing stays set so resume knows to restart.
   No-op if not playing.

   The playback POSITION is captured here and restored by cdaudio_resume, so the
   music picks up where it left off rather than snapping back to the top of the
   track. No door needs that any more — every transition now cdaudio_stop()s and
   the destination room starts its track fresh — but the MID-ROOM streamers do:
   sound_bank_select() and room_arena_load() suspend around their reads while the
   player stands in a room with the score playing, and without the position
   restore each one would jump the music back to the top. */
void cdaudio_suspend(void) {
    if (!cd_audio_playing) return;

    uint32_t sec;
    int      trk;
    cd_resume_sector = read_position(&sec, &trk) ? sec : 0;

    /* BLOCKING stop: CdControlB waits for the command to complete, so the
       drive is actually halted before we issue data reads. A non-blocking
       CdControl(CdlStop) returns immediately and the still-streaming drive
       corrupts the subsequent CdRead (garbage TIM -> LoadImage crash). */
    CdControlB(CdlStop, NULL, NULL);
}

void cdaudio_resume(void) {
    if (!cd_audio_playing) return;

    /* Refuse a resume point that is not plausibly inside the track: a bad
       position read would otherwise strand playback in the lead-out or in the
       next track. Falling back to the track start is the old behaviour, which
       is merely unmusical rather than broken. */
    uint32_t at    = cd_resume_sector;
    uint32_t start = cd_loc_valid ? loc_to_sector(&cd_track_loc) : 0;
    if (at < start || (cd_end_sector && at >= cd_end_sector))
        at = 0;

    cd_resume_sector = 0;
    issue_play_from(at);
}

void cdaudio_stop(void) {
    if (!cd_audio_playing) return;
    cd_loop_mode     = 0;
    CdControl(CdlStop, NULL, NULL);
    cd_audio_playing = 0;
}

void cdaudio_set_volume(int left, int right) {
    SpuSetCommonCDVolume(left & 0x7FFF, right & 0x7FFF);
}

/* Set the DRIVE mixer level directly, 0-255 with CD_MIX_FULL (128) = 100%.
   This is the stage that actually governs how loud CD-DA comes out — see the
   CD_MIX_* note at the top of this file for why the SPU-side
   cdaudio_set_volume() is not the one to ramp. Used by the opening sequence to
   fade the music down under its last block of text.

   Nothing has to put the level back: cdaudio_play() re-asserts the per-track
   level on every start, so the next room's music comes up at its own volume
   whatever this was left at. */
void cdaudio_set_mix(int level) {
    if (level < 0)   level = 0;
    if (level > 255) level = 255;
    CdlATV mix = { (uint8_t)level, 0, (uint8_t)level, 0 };
    CdMix(&mix);
}

int cdaudio_mix_full(void) { return CD_MIX_FULL; }
