#include <stdint.h>
#include <psxcd.h>
#include <psxspu.h>
#include "cdaudio.h"

static int cd_audio_playing = 0;

void cdaudio_init(void) {
    /* CD already initialised by level_init() via CdInit()
       Set CD audio volume to full through SPU mixer */
    SpuSetCommonCDVolume(0x7FFF, 0x7FFF);
    cd_audio_playing = 0;
}

void cdaudio_play(int track, int loop) {
    CdlLOC loc;
    uint8_t param[1];

    /* Convert track number (1-based) to CD location
       CdIntToPos encodes a sector position into MM:SS:FF */
    CdIntToPos(0, &loc);

    /* Play the track — CdPlay will read the track table and start from
       the beginning of the specified track. */
    param[0] = (uint8_t)track;
    CdControl(CdlSetloc, &loc, NULL);
    CdControl(CdlPlay, param, NULL);

    cd_audio_playing = 1;
}

void cdaudio_stop(void) {
    if (!cd_audio_playing) return;
    CdControl(CdlStop, NULL, NULL);
    cd_audio_playing = 0;
}

void cdaudio_set_volume(int left, int right) {
    SpuSetCommonCDVolume(left & 0x7FFF, right & 0x7FFF);
}
