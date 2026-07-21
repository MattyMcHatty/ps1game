#!/usr/bin/env python3
"""
wav_to_vag.py
Converts a WAV file to PS1 VAG format (Sony ADPCM).

Requirements:
    pip install Pillow  (already installed)
    No extra dependencies needed beyond standard library

Usage:
    python3 wav_to_vag.py sound.wav
    python3 wav_to_vag.py sound.wav --out sound.vag --rate 22050

Options:
    --out FILE    Output filename (default: same name with .vag)
    --rate N      Sample rate (8000, 11025, 22050, 44100) default: 22050
    --name STR    VAG name tag (max 16 chars, default: filename)

Notes:
    - Input must be mono WAV. Stereo will be mixed down to mono.
    - 16-bit PCM WAV only.
    - Lower sample rates (11025, 22050) save SPU RAM and sound fine for effects.
    - VAG ADPCM compresses 28 samples into 16 bytes (ratio ~3.5:1).
"""

import sys
import os
import struct
import wave

# -----------------------------------------------------------------------
# ADPCM encoding tables (Sony SPU ADPCM)
# -----------------------------------------------------------------------

# Filter coefficients (fixed point, factor = 32)
FILTER_K1 = [0,  60,  115,  98,  122]
FILTER_K2 = [0,   0,  -52, -55,  -60]

# Quantisation step sizes — RANGE_STEP[n] = 1 << (12 - n)
# For shift factor n, the SPU decodes each nibble as nibble << (12 - n),
# so the encoder quantises with this step to match.
RANGE_STEP = [4096, 2048, 1024, 512, 256, 128, 64, 32, 16, 8, 4, 2, 1]


def encode_adpcm_block(samples, prev1, prev2):
    """Encode 28 samples into a 16-byte SPU ADPCM block.
    Returns (encoded_bytes, new_prev1, new_prev2)
    samples: list of 28 int16 values
    """
    best_error = float('inf')
    best_range = 0
    best_filter = 0
    best_nibbles = []

    # Try each filter and range combination, pick lowest error
    for filt in range(5):
        k1 = FILTER_K1[filt]
        k2 = FILTER_K2[filt]

        for rng in range(13):
            step = RANGE_STEP[rng]
            nibbles = []
            p1, p2 = prev1, prev2
            total_error = 0

            for s in samples:
                # Predict — filter coefficients are in factor-64 notation
                predict = (p1 * k1 + p2 * k2) >> 6

                # Residual
                residual = s - predict

                # Quantise to 4-bit signed (-8 to 7)
                q = int(residual / step)
                q = max(-8, min(7, q))
                nibbles.append(q)

                # Reconstruct
                reconstructed = predict + q * step
                reconstructed = max(-32768, min(32767, reconstructed))

                error = (s - reconstructed) ** 2
                total_error += error

                p2 = p1
                p1 = reconstructed

            if total_error < best_error:
                best_error = total_error
                best_range = rng
                best_filter = filt
                best_nibbles = nibbles[:]

    # Encode using best parameters
    header_byte1 = (best_range & 0xF) | ((best_filter & 0xF) << 4)
    header_byte2 = 0x00  # flags (set later by caller)

    # Pack nibbles into bytes (2 nibbles per byte, low nibble first)
    packed = []
    for i in range(0, 28, 2):
        lo = best_nibbles[i] & 0xF
        hi = best_nibbles[i + 1] & 0xF if i + 1 < len(best_nibbles) else 0
        packed.append(lo | (hi << 4))

    # Reconstruct final prev values
    p1, p2 = prev1, prev2
    k1 = FILTER_K1[best_filter]
    k2 = FILTER_K2[best_filter]
    step = RANGE_STEP[best_range]

    for q in best_nibbles:
        predict = (p1 * k1 + p2 * k2) >> 6
        reconstructed = predict + q * step
        reconstructed = max(-32768, min(32767, reconstructed))
        p2 = p1
        p1 = reconstructed

    block = bytes([header_byte1, header_byte2] + packed)
    return block, p1, p2


def encode_vag(samples, sample_rate, name, loop=False):
    """Encode PCM samples to VAG format.
    samples: list of int16
    loop: if True, the whole sample loops forever (first block = loop start,
          last block = loop end + repeat); the SPU keeps playing it until the
          voice is keyed off. Otherwise it is a one-shot.
    Returns VAG bytes.
    """
    # Pad samples to multiple of 28
    while len(samples) % 28 != 0:
        samples.append(0)

    blocks = []
    prev1, prev2 = 0, 0

    n_blocks = len(samples) // 28
    for i in range(n_blocks):
        block_samples = samples[i * 28:(i + 1) * 28]
        block, prev1, prev2 = encode_adpcm_block(block_samples, prev1, prev2)

        # ADPCM block flag byte (2nd byte):
        #   loop:      first = 0x06 (loop start), last = 0x03 (loop end+repeat)
        #   one-shot:  last  = 0x01 (end)
        if loop:
            if   i == 0:            flag = 0x06
            elif i == n_blocks - 1: flag = 0x03
            else:                   flag = 0x00
        else:
            flag = 0x01 if i == n_blocks - 1 else 0x00
        block = block[:1] + bytes([flag]) + block[2:]

        blocks.append(block)

    # One-shots get a trailing silent end block; a loop repeats instead.
    if not loop:
        blocks.append(bytes([0x00, 0x07] + [0x00] * 14))

    body = b''.join(blocks)

    # VAG header (48 bytes)
    data_size = len(body)
    name_bytes = name[:16].encode('ascii').ljust(16, b'\x00')

    header = struct.pack('>4sIII',
        b'VAGp',          # magic
        0x00000020,       # version
        0x00000000,       # reserved
        data_size,        # data size
    )
    header += struct.pack('>I', sample_rate)
    header += b'\x00' * 12   # reserved
    header += name_bytes

    return header + body


# -----------------------------------------------------------------------
# WAV reader
# -----------------------------------------------------------------------

def read_wav(filepath):
    """Read a WAV file and return (samples_int16, sample_rate)."""
    with wave.open(filepath, 'rb') as w:
        channels   = w.getnchannels()
        sampwidth  = w.getsampwidth()
        framerate  = w.getframerate()
        nframes    = w.getnframes()
        raw        = w.readframes(nframes)

    print(f"  Channels:    {channels}")
    print(f"  Sample rate: {framerate} Hz")
    print(f"  Bit depth:   {sampwidth * 8}-bit")
    print(f"  Frames:      {nframes}")

    if sampwidth == 2:
        fmt = f'<{nframes * channels}h'
        all_samples = list(struct.unpack(fmt, raw))
    elif sampwidth == 1:
        # 8-bit WAV is unsigned, convert to signed 16-bit
        all_samples = [(b - 128) * 256 for b in raw]
        if channels > 1:
            all_samples = all_samples * 1  # already flat
    elif sampwidth == 3:
        # 24-bit
        all_samples = []
        for i in range(0, len(raw), 3 * channels):
            for c in range(channels):
                b = raw[i + c*3:i + c*3 + 3]
                val = struct.unpack('<i', b + (b'\xff' if b[2] & 0x80 else b'\x00'))[0]
                all_samples.append(val >> 8)
    else:
        raise ValueError(f"Unsupported sample width: {sampwidth} bytes")

    # Mix down to mono if stereo
    if channels == 2:
        print("  Mixing stereo to mono...")
        mono = []
        for i in range(0, len(all_samples), 2):
            mixed = (all_samples[i] + all_samples[i + 1]) // 2
            mono.append(max(-32768, min(32767, mixed)))
        all_samples = mono
    elif channels > 2:
        raise ValueError("Only mono and stereo WAV supported")

    # Resample if needed (simple linear interpolation)
    return all_samples, framerate


def resample(samples, src_rate, dst_rate):
    """Simple linear interpolation resampler."""
    if src_rate == dst_rate:
        return samples

    ratio = src_rate / dst_rate
    out_len = int(len(samples) / ratio)
    out = []

    for i in range(out_len):
        pos = i * ratio
        idx = int(pos)
        frac = pos - idx

        if idx + 1 < len(samples):
            val = int(samples[idx] * (1 - frac) + samples[idx + 1] * frac)
        else:
            val = samples[idx] if idx < len(samples) else 0

        out.append(max(-32768, min(32767, val)))

    return out


# -----------------------------------------------------------------------
# Main
# -----------------------------------------------------------------------

def main():
    target_rate = 22050
    out_file    = None
    vag_name    = None
    loop        = False

    args = sys.argv[1:]
    if not args or args[0] in ('-h', '--help'):
        print("Usage: python3 wav_to_vag.py sound.wav [options]")
        print("")
        print("Options:")
        print("  --out FILE    Output filename (default: same name .vag)")
        print("  --rate N      Target sample rate (default: 22050)")
        print("  --name STR    VAG name tag (default: filename, max 16 chars)")
        print("")
        print("Recommended rates:")
        print("  44100  - high quality music")
        print("  22050  - good quality effects (recommended)")
        print("  11025  - lower quality, saves SPU RAM")
        print("   8000  - voice/simple tones only")
        sys.exit(0)

    wav_file = args[0]
    i = 1
    while i < len(args):
        if args[i] == '--out'  and i+1 < len(args): out_file    = args[i+1]; i += 2
        elif args[i] == '--rate' and i+1 < len(args): target_rate = int(args[i+1]); i += 2
        elif args[i] == '--name' and i+1 < len(args): vag_name    = args[i+1]; i += 2
        elif args[i] == '--loop': loop = True; i += 1
        else: i += 1

    if not os.path.exists(wav_file):
        print(f"Error: file not found: {wav_file}")
        sys.exit(1)

    if out_file is None:
        out_file = os.path.splitext(wav_file)[0] + ".vag"

    if vag_name is None:
        vag_name = os.path.splitext(os.path.basename(wav_file))[0][:16]

    print(f"Reading: {wav_file}")
    samples, src_rate = read_wav(wav_file)
    print(f"  Samples: {len(samples)}")

    if src_rate != target_rate:
        print(f"Resampling {src_rate} Hz -> {target_rate} Hz...")
        samples = resample(samples, src_rate, target_rate)
        print(f"  Resampled to {len(samples)} samples")

    duration = len(samples) / target_rate
    print(f"  Duration: {duration:.2f} seconds")
    print(f"  SPU RAM needed: ~{len(samples) // 28 * 16 / 1024:.1f} KB")

    print(f"Encoding to VAG{' (looping)' if loop else ''}...")
    vag_data = encode_vag(samples, target_rate, vag_name, loop=loop)

    print(f"Writing: {out_file}")
    with open(out_file, 'wb') as f:
        f.write(vag_data)

    size = os.path.getsize(out_file)
    print(f"  File size: {size} bytes ({size/1024:.1f} KB)")
    print(f"  VAG name:  {vag_name}")
    print(f"  Rate:      {target_rate} Hz")
    print()
    print("Done! Add to disc.xml and load with the SPU system.")

if __name__ == "__main__":
    main()
