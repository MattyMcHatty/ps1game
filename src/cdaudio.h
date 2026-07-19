#ifndef CDAUDIO_H
#define CDAUDIO_H

/* CD audio track — Track 1 is data, Track 2+ are audio */
#define CDAUDIO_MUSIC_TRACK     2   /* default music (kitchen / delivery) */
#define CDAUDIO_RECEPTION_TRACK 3   /* reception music */
#define CDAUDIO_PIANO_TRACK     4   /* piano room music */

void cdaudio_init(void);
void cdaudio_play(int track, int loop);
void cdaudio_update(void);
void cdaudio_suspend(void);   /* halt CD-DA so the drive is free for data reads */
void cdaudio_resume(void);    /* restart CD-DA after a suspend */
void cdaudio_stop(void);
void cdaudio_set_volume(int left, int right);

#endif
