#!/usr/bin/env python3
"""
mp3_to_wav.py
Converts MP3 files to WAV format suitable for wav_to_vag.py

Requirements:
    pip install pydub
    Also requires ffmpeg - see instructions below

Installing ffmpeg on Windows:
    1. Go to https://ffmpeg.org/download.html
    2. Click Windows builds from gyan.dev
    3. Download ffmpeg-release-essentials.zip
    4. Extract it somewhere e.g. C:\ffmpeg
    5. Add C:\ffmpeg\bin to your PATH environment variable
       (same way you added PSn00bSDK\bin to PATH earlier)
    6. Open a new Command Prompt and run: ffmpeg -version

Usage:
    py mp3_to_wav.py sound.mp3
    py mp3_to_wav.py sound.mp3 --out sound.wav --rate 22050
    py mp3_to_wav.py *.mp3

Options:
    --out FILE    Output filename (default: same name with .wav)
    --rate N      Output sample rate (default: 22050)
                  Recommended: 22050 for sound effects, 44100 for music

Then convert the WAV to VAG:
    py wav_to_vag.py sound.wav
"""

import sys
import os

def check_pydub():
    try:
        from pydub import AudioSegment
        return True
    except Exception as e:
        print(f"pydub import error: {e}")
        return False

def check_ffmpeg():
    import subprocess
    try:
        subprocess.run(['ffmpeg', '-version'],
                      capture_output=True, check=True)
        return True
    except (subprocess.CalledProcessError, FileNotFoundError):
        return False

def convert_mp3_to_wav(mp3_path, out_path, target_rate):
    from pydub import AudioSegment

    print(f"Reading: {mp3_path}")
    audio = AudioSegment.from_mp3(mp3_path)

    print(f"  Channels:    {audio.channels}")
    print(f"  Sample rate: {audio.frame_rate} Hz")
    print(f"  Duration:    {len(audio)/1000:.2f} seconds")

    # Mix down to mono
    if audio.channels > 1:
        print("  Mixing to mono...")
        audio = audio.set_channels(1)

    # Resample if needed
    if audio.frame_rate != target_rate:
        print(f"  Resampling {audio.frame_rate} Hz -> {target_rate} Hz...")
        audio = audio.set_frame_rate(target_rate)

    # Ensure 16-bit
    audio = audio.set_sample_width(2)

    print(f"Writing: {out_path}")
    audio.export(out_path, format='wav')
    size = os.path.getsize(out_path)
    print(f"  File size: {size/1024:.1f} KB")
    print(f"  Done!")
    print()

def main():
    target_rate = 22050
    out_file    = None

    args = sys.argv[1:]
    if not args or args[0] in ('-h', '--help'):
        print("Usage: py mp3_to_wav.py sound.mp3 [options]")
        print("")
        print("Options:")
        print("  --out FILE    Output filename (default: same name .wav)")
        print("  --rate N      Target sample rate (default: 22050)")
        print("")
        print("Then run:")
        print("  py wav_to_vag.py sound.wav")
        sys.exit(0)

    # Check dependencies
    if not check_pydub():
        print("Error: pydub is not installed.")
        print("Run: pip install pydub")
        sys.exit(1)

    if not check_ffmpeg():
        print("Error: ffmpeg is not found.")
        print("")
        print("Install ffmpeg:")
        print("  1. Go to https://ffmpeg.org/download.html")
        print("  2. Click 'Windows builds from gyan.dev'")
        print("  3. Download ffmpeg-release-essentials.zip")
        print("  4. Extract to C:\\ffmpeg")
        print("  5. Add C:\\ffmpeg\\bin to your PATH")
        print("  6. Open a new Command Prompt and try again")
        sys.exit(1)

    # Parse args
    mp3_files = []
    i = 0
    while i < len(args):
        if args[i] == '--rate' and i+1 < len(args):
            target_rate = int(args[i+1]); i += 2
        elif args[i] == '--out' and i+1 < len(args):
            out_file = args[i+1]; i += 2
        else:
            mp3_files.append(args[i]); i += 1

    if not mp3_files:
        print("Error: no MP3 files specified")
        sys.exit(1)

    for mp3_path in mp3_files:
        if not os.path.exists(mp3_path):
            print(f"Warning: file not found: {mp3_path}")
            continue

        if out_file and len(mp3_files) == 1:
            wav_path = out_file
        else:
            wav_path = os.path.splitext(mp3_path)[0] + ".wav"

        convert_mp3_to_wav(mp3_path, wav_path, target_rate)

    print("All done! Now run wav_to_vag.py on each WAV file.")
    print("Example:")
    for mp3_path in mp3_files:
        wav_path = os.path.splitext(mp3_path)[0] + ".wav"
        print(f"  py wav_to_vag.py {wav_path}")

if __name__ == "__main__":
    main()
