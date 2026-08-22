#ifndef CDAUDIO_H
#define CDAUDIO_H

/* CD audio track — Track 1 is data, Track 2+ are audio */
#define CDAUDIO_MUSIC_TRACK     2   /* default music (kitchen / delivery) */
#define CDAUDIO_RECEPTION_TRACK 3   /* reception music */
#define CDAUDIO_PIANO_TRACK     4   /* piano room music */
#define CDAUDIO_ANZU_TRACK      5   /* piano room music once the Anzu Tablet is solved */
#define CDAUDIO_COURTYARD_TRACK 6   /* garden courtyard music */
#define CDAUDIO_FOUNTAIN_TRACK  7   /* fountain square music */
/* Hadad's stalk. The only track in the game that does NOT loop back to its own
   start: the first 4.015 s are a one-off swell that must not be heard again, so
   every repeat begins at CDAUDIO_STALKER_LOOP_SEC instead. See the loop-offset
   note in cdaudio.c — the offset is a property of the TRACK, so it lives beside
   the track number here rather than at the call site in hadad.c. */
#define CDAUDIO_STALKER_TRACK   8   /* Hadad's stalker music (20.0 s) */

void cdaudio_init(void);
void cdaudio_play(int track, int loop);
void cdaudio_update(void);
void cdaudio_suspend(void);   /* halt CD-DA so the drive is free for data reads */
void cdaudio_resume(void);    /* restart CD-DA after a suspend */
void cdaudio_stop(void);
void cdaudio_set_volume(int left, int right);
void cdaudio_set_mix(int level);   /* drive mixer 0-255; the stage that is audible */
int  cdaudio_mix_full(void);       /* the 100% level for cdaudio_set_mix           */

#endif
