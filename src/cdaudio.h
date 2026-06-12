#ifndef CDAUDIO_H
#define CDAUDIO_H

/* CD audio track — Track 1 is data, Track 2+ are audio */
#define CDAUDIO_MUSIC_TRACK 2

void cdaudio_init(void);
void cdaudio_play(int track, int loop);
void cdaudio_update(void);
void cdaudio_stop(void);
void cdaudio_set_volume(int left, int right);

#endif
