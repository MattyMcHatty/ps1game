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
/* ASAG'S ARENA. >>> THE TRACK IS RESERVED; THE AUDIO IS NOT ON THE DISC YET.
   <<< Adding it is one <track type="audio"> line at the foot of disc.xml, after
   track 8 and in that order — the track NUMBER is the line's position in that
   file and nothing else, so a track inserted rather than appended renumbers
   every one after it and every room in the game plays the wrong music.

   It must be 44100 Hz 16-bit STEREO (Redbook). mkpsxiso accepts anything and
   silently converts, and what comes out of a mono source is upsampled mono;
   music/mp3_to_wav.py is NOT the tool (it is the sound-effect path and always
   mixes to mono). Convert with ffmpeg or pydub straight to 44100 stereo.

   >>> AND IT IS THE BOSS'S TRACK, NOT THE ROOM'S. <<< main.c's STATE_LOADING
   branch calls cdaudio_stop() for this room rather than cdaudio_play(), so the
   arena is SILENT before the encounter and after it, and the encounter's phase
   machine starts this at the beat the brief names. Replacing the stop with
   nothing would be a bug, not a simplification: a title-screen load or a debug
   level-select jump into this room does not pass through the drop's own stop and
   would arrive with The Hatch's music still playing. */
#define CDAUDIO_ASAG_TRACK      9   /* Asag's arena (RESERVED — see above) */

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
