#!/usr/bin/env python3
"""Cut the near-silent tail (and optionally lead) off a 16-bit wav IN PLACE.

    py trim_wav.py gr_shot.wav              # tail only
    py trim_wav.py DIE.wav --lead           # tail AND lead

WHY THIS EXISTS
    SPU RAM is full (tools/ADDING_A_SOUND.txt STEP 0), and a clip's cost is
    its LENGTH - silence costs exactly what sound costs. Several resident
    clips ended in over a second of decay sitting more than 34 dB under their
    own peak, inaudible on a TV but paid for in every bank. Sharur's two
    sounds (Nov 2026) were paid for by cutting five of those tails.

WHAT IT DOES
    "Silent" is relative to the clip's OWN peak: a sample is quiet while
    |s| <= peak * --thresh (default 0.02, i.e. -34 dB). The cut lands --pad
    seconds after the last loud sample, and the final --fade seconds before
    the cut ramp linearly to zero, so the end is a fade rather than a click.
    --lead does the same at the front with a 5 ms fade-in; note that moves
    the clip's attack EARLIER relative to whatever fires it, which is why it
    is opt-in.

    Sample rate and level are untouched. Re-run wav_to_vag.py afterwards at
    the clip's own rate - this only touches the wav, and git holds the old one.
"""

import argparse
import struct
import sys
import wave


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('wav')
    ap.add_argument('--thresh', type=float, default=0.02,
                    help='quiet = |s| <= peak*thresh (default 0.02 = -34 dB)')
    ap.add_argument('--pad', type=float, default=0.05,
                    help='seconds kept after the last loud sample (default 0.05)')
    ap.add_argument('--fade', type=float, default=0.08,
                    help='fade-out length ending at the cut (default 0.08 s)')
    ap.add_argument('--lead', action='store_true',
                    help='also trim near-silence at the start')
    a = ap.parse_args()

    w = wave.open(a.wav, 'rb')
    ch, sw, rate, n = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
    raw = w.readframes(n)
    w.close()
    if sw != 2:
        sys.exit('16-bit PCM only')
    s = list(struct.unpack('<%dh' % (n * ch), raw))
    frames = [s[i * ch:(i + 1) * ch] for i in range(n)]

    peak = max(max(abs(x) for x in f) for f in frames) or 1
    th = peak * a.thresh
    loud = [i for i, f in enumerate(frames) if max(abs(x) for x in f) > th]
    first, last = loud[0], loud[-1]

    end = min(n, last + 1 + int(a.pad * rate))
    start = max(0, first - int(0.01 * rate)) if a.lead else 0
    out = frames[start:end]

    fl = min(len(out), int(a.fade * rate))
    for k in range(fl):                       # fade-out into the cut
        g = (fl - k) / float(fl + 1)
        out[len(out) - fl + k] = [int(x * g) for x in out[len(out) - fl + k]]
    if a.lead and start > 0:
        fi = min(len(out), int(0.005 * rate))
        for k in range(fi):
            g = (k + 1) / float(fi + 1)
            out[k] = [int(x * g) for x in out[k]]

    o = wave.open(a.wav, 'wb')
    o.setnchannels(ch)
    o.setsampwidth(2)
    o.setframerate(rate)
    o.writeframes(struct.pack('<%dh' % (len(out) * ch), *[x for f in out for x in f]))
    o.close()

    def spu(fr):                               # 28 samples -> 16-byte block, 64-aligned
        return (((fr + 27) // 28) * 16 + 63) & ~63
    print('%s: %.2fs -> %.2fs  (lead -%.2fs, tail -%.2fs)  SPU ~%d -> ~%d (saves ~%d)' % (
        a.wav, n / rate, len(out) / rate, start / rate, (n - end) / rate,
        spu(n), spu(len(out)), spu(n) - spu(len(out))))


if __name__ == '__main__':
    main()
