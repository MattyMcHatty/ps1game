#!/usr/bin/env python3
"""Raise the level of a 16-bit PCM wav IN PLACE, with a soft limiter.

    py gain_wav.py explode.wav --gain 1.5

WHY THIS EXISTS
    The SPU gives no runtime headroom. sound.c plays every voice at
    SPU_CH_VOL 0x3fff, which is the maximum in volume mode (the register's
    value is doubled internally), and nothing sets the common master volume
    away from SpuInit's max either. So "make this effect louder" can only
    mean "re-gain the sample", and this is the tool that does it.

THE LIMITER, AND WHY IT IS NOT A CLIP
    Most of the game's clips already peak at or near 0 dBFS, so a straight
    1.5x multiply would flat-top every peak and buzz. Instead, samples below
    --knee (as a fraction of full scale) are scaled by the full gain and left
    alone; above it the curve bends over with a tanh and ASYMPTOTES to full
    scale, so it can never clip however hard it is driven. The quiet body of
    the clip therefore gets the whole 50%, the peaks get what is left, and
    what you actually hear rise is the RMS.

    The gain a clip really received is printed as an RMS delta. A clip that
    was already slammed will report well under the asked-for figure; that is
    the honest number and no tool can do better with the peaks it has.

    Length and sample rate are untouched, so the VAG's SPU cost after
    re-converting is byte-identical and no budget changes.
    Re-run wav_to_vag.py afterwards — this only touches the wav.
"""

import argparse
import math
import struct
import sys
import wave

FULL = 32767.0


def rms_db(samples):
    if not samples:
        return -99.0
    acc = sum(float(s) * float(s) for s in samples)
    r = math.sqrt(acc / len(samples))
    return 20.0 * math.log10(r / FULL) if r > 0 else -99.0


def peak_db(samples):
    p = max(max(samples), -min(samples)) if samples else 0
    return (20.0 * math.log10(p / FULL) if p > 0 else -99.0), p


def apply_gain(samples, gain, knee):
    out = []
    for s in samples:
        y = (s / FULL) * gain
        sign = -1.0 if y < 0 else 1.0
        u = abs(y)
        if u > knee:
            u = knee + (1.0 - knee) * math.tanh((u - knee) / (1.0 - knee))
        v = int(round(sign * u * FULL))
        out.append(max(-32768, min(32767, v)))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("wav")
    ap.add_argument("--gain", type=float, default=1.5,
                    help="linear multiplier, 1.5 = +50%% (default 1.5)")
    ap.add_argument("--knee", type=float, default=0.7,
                    help="fraction of full scale the limiter starts bending "
                         "at (default 0.7)")
    ap.add_argument("--dry-run", action="store_true",
                    help="report what would change and write nothing")
    args = ap.parse_args()

    with wave.open(args.wav, "rb") as w:
        if w.getsampwidth() != 2:
            sys.exit("%s: not 16-bit PCM" % args.wav)
        if w.getnchannels() != 1:
            sys.exit("%s: not mono — convert first" % args.wav)
        rate = w.getframerate()
        nframes = w.getnframes()
        raw = w.readframes(nframes)

    samples = list(struct.unpack("<%dh" % (len(raw) // 2), raw))
    out = apply_gain(samples, args.gain, args.knee)

    pdb_a, pk_a = peak_db(samples)
    pdb_b, pk_b = peak_db(out)
    rdb_a, rdb_b = rms_db(samples), rms_db(out)

    print("%-14s %5d Hz  %6.3f s" % (args.wav, rate, nframes / float(rate)))
    print("   peak  %6.1f -> %6.1f dBFS   (%d -> %d)" % (pdb_a, pdb_b, pk_a, pk_b))
    print("   RMS   %6.1f -> %6.1f dBFS   = x%.2f of the x%.2f asked for"
          % (rdb_a, rdb_b, 10.0 ** ((rdb_b - rdb_a) / 20.0), args.gain))

    if args.dry_run:
        print("   (dry run, nothing written)")
        return

    with wave.open(args.wav, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(struct.pack("<%dh" % len(out), *out))


if __name__ == "__main__":
    main()
