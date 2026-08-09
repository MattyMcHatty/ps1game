#!/usr/bin/env python3
"""
crush_wav.py
Deliberately degrades a mono 16-bit WAV: band-limits it, drives it into soft
clipping and quantises it to a handful of bits. The point is the SOUND, not the
file size — a clip run through this gets the thin, gritty, overdriven quality of
a 90s survival-horror voice sample.

Like gain_wav.py it rewrites the WAV **in place** (git holds the previous
version) so the existing pipeline follows unchanged:

    py mp3_to_wav.py "My Voice.mp3" --out myvoice.wav --rate 8000
    py crush_wav.py myvoice.wav --bits 6 --drive 2.5
    py wav_to_vag.py myvoice.wav --rate 8000

Options:
    --bits N     quantisation depth, 1-16 (default 6). This is the grain.
    --drive X    pre-gain into a soft clipper, 1.0 = clean (default 2.5)
    --hp HZ      one-pole high-pass, thins the body out (default 300)
    --lp HZ      one-pole low-pass, kills the air (default 3400)
    --hold N     sample-and-hold by N, for extra aliasing buzz (default 1 = off)

ORDER MATTERS: band-limit, then drive, then quantise. Quantising first and
filtering after would smooth the very steps that make the grain audible.

NOTE the sample rate is NOT changed here — set that in mp3_to_wav.py, because
it is also what decides the clip's SPU cost (tools/ADDING_A_SOUND.txt STEP 3).
"""

import sys
import math
import wave
import struct


def one_pole(samples, cutoff, rate, highpass):
    """One-pole RC filter. Gentle (6 dB/oct) on purpose: a steep filter sounds
    like a filter, a lazy one sounds like a cheap speaker."""
    if cutoff <= 0:
        return samples
    dt = 1.0 / rate
    rc = 1.0 / (2.0 * math.pi * cutoff)
    out = [0.0] * len(samples)
    if highpass:
        a = rc / (rc + dt)
        prev_in = prev_out = 0.0
        for i, x in enumerate(samples):
            prev_out = a * (prev_out + x - prev_in)
            prev_in = x
            out[i] = prev_out
    else:
        a = dt / (rc + dt)
        prev_out = 0.0
        for i, x in enumerate(samples):
            prev_out += a * (x - prev_out)
            out[i] = prev_out
    return out


def main(argv):
    if not argv:
        print(__doc__)
        return 1

    path = argv[0]
    opts = {"--bits": 6, "--drive": 2.5, "--hp": 300.0, "--lp": 3400.0, "--hold": 1}
    i = 1
    while i < len(argv) - 1:
        if argv[i] in opts:
            opts[argv[i]] = float(argv[i + 1])
        i += 2

    bits = int(opts["--bits"])
    drive = float(opts["--drive"])
    hold = max(1, int(opts["--hold"]))

    with wave.open(path, "rb") as w:
        if w.getsampwidth() != 2 or w.getnchannels() != 1:
            print("Error: expected mono 16-bit WAV (run mp3_to_wav.py first)")
            return 1
        rate = w.getframerate()
        n = w.getnframes()
        raw = w.readframes(n)

    x = [s / 32768.0 for s in struct.unpack("<%dh" % n, raw)]
    rms_in = math.sqrt(sum(v * v for v in x) / max(1, n))

    x = one_pole(x, opts["--hp"], rate, True)
    x = one_pole(x, opts["--lp"], rate, False)

    # Soft clip. Normalised by tanh(drive) so a harder drive gets dirtier
    # without simply getting louder — there is no runtime volume knob in the
    # engine (ADDING_A_SOUND.txt STEP 1B), so level has to stay deliberate.
    if drive > 1.0:
        k = math.tanh(drive)
        x = [math.tanh(v * drive) / k for v in x]

    if hold > 1:
        for i in range(0, len(x), hold):
            x[i:i + hold] = [x[i]] * len(x[i:i + hold])

    levels = float(1 << (bits - 1))
    out = []
    for v in x:
        q = math.floor(v * levels + 0.5) / levels
        s = int(max(-1.0, min(1.0, q)) * 32767)
        out.append(s)

    rms_out = math.sqrt(sum((v / 32768.0) ** 2 for v in out) / max(1, len(out)))

    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(struct.pack("<%dh" % len(out), *out))

    print("%s: %d Hz, %d samples" % (path, rate, len(out)))
    print("  band %g-%g Hz, drive %.2f, %d-bit, hold %d"
          % (opts["--hp"], opts["--lp"], drive, bits, hold))
    print("  RMS %.4f -> %.4f (%+.1f dB)"
          % (rms_in, rms_out,
             20 * math.log10(rms_out / rms_in) if rms_in and rms_out else 0.0))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
