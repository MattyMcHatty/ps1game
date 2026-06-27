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
#define CD_STALL_LIMIT      2   /* consecutive non-advancing polls = track ended */
#define CD_NOTPLAY_LIMIT    2   /* consecutive "not playing" status reads = track ended */
#define CD_FAIL_LIMIT       4   /* consecutive failed position reads = drive stopped */
#define CD_END_MARGIN      75   /* restart this many sectors (~1s) before the lead-out */

static int           cd_audio_playing = 0;
static int           cd_track_num     = 0;
static int           cd_loop_mode     = 0;

/* Resolved start location of the music track (from the TOC). */
static CdlLOC        cd_track_loc;
static int           cd_loc_valid     = 0;
/* Absolute sector at which the track ends (disc lead-out). 0 if unknown. */
static uint32_t      cd_end_sector    = 0;

/* Position-polling state. */
static int           poll_tick        = 0;
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

/* Seek to the resolved track start and begin CD-DA playback. CdlModeDA selects
   audio mode; speed bit stays clear (CD-DA must play at 1x). No CdlModeAP — we
   want continuous playback to end of disc and detect the end by polling. */
static void issue_play(void) {
    uint8_t mode = CdlModeDA;
    CdControl(CdlSetmode, &mode, NULL);
    if (cd_loc_valid)
        CdControl(CdlSetloc, &cd_track_loc, NULL);
    CdControl(CdlPlay, NULL, NULL);

    /* Reset poll state so the seek/spinup window isn't mistaken for a stall. */
    poll_tick     = 0;
    grace_polls   = CD_GRACE_POLLS;
    stall_count   = 0;
    notplay_count = 0;
    fail_count    = 0;
    last_sector   = 0;
}

void cdaudio_init(void) {
    /* Route CD audio into the SPU mixer at full volume... */
    SpuSetCommonCDVolume(0x7FFF, 0x7FFF);
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

    cd_audio_playing = 1;
    cd_track_num     = track;
    cd_loop_mode     = loop;
    issue_play();
}

void cdaudio_update(void) {
    if (!cd_audio_playing || !cd_loop_mode) return;

    if (++poll_tick < CD_POLL_INTERVAL) return;
    poll_tick = 0;

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
        if (++notplay_count >= CD_NOTPLAY_LIMIT) { issue_play(); return; }
    } else if (playing == 1) {
        notplay_count = 0;
    }

    /* Signals 2-5 come from the physical position. */
    uint32_t cur = 0;
    int      cur_track = 0;
    if (!read_position(&cur, &cur_track)) {
        /* Signal 2: position reads keep failing — drive has likely stopped. */
        if (++fail_count >= CD_FAIL_LIMIT) { issue_play(); return; }
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
        issue_play();
        return;
    }

    /* Signal 4: playback left our track (into the next track or the lead-out).
       cur_track == 0 is treated as "unknown" and ignored. */
    if (cur_track != 0 && cur_track != cd_track_num) {
        issue_play();
        return;
    }

    /* Signal 5: position stopped advancing — drive reached end of disc. */
    if (cur <= last_sector) {
        if (++stall_count >= CD_STALL_LIMIT) {
            issue_play();
            return;
        }
    } else {
        stall_count = 0;
    }
    last_sector = cur;
}

/* Temporarily halt CD-DA so the drive is free for data reads (CdRead) mid-game.
   The original design loaded all assets at startup precisely to avoid competing
   with CD-DA; per-room texture streaming needs mid-game reads, so it must bracket
   them with suspend/resume or the drive hangs (reading data while audio streams).
   cd_audio_playing stays set so resume knows to restart. No-op if not playing. */
void cdaudio_suspend(void) {
    if (cd_audio_playing) {
        /* BLOCKING stop: CdControlB waits for the command to complete, so the
           drive is actually halted before we issue data reads. A non-blocking
           CdControl(CdlStop) returns immediately and the still-streaming drive
           corrupts the subsequent CdRead (garbage TIM -> LoadImage crash). */
        CdControlB(CdlStop, NULL, NULL);
    }
}

void cdaudio_resume(void) {
    if (cd_audio_playing)
        issue_play();   /* re-seek to the track and restart DA playback */
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
