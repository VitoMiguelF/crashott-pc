"""Level of each channel of a WAV dump (CRASH_AUDIO_DUMP), to check a surround mix without a receiver.

    python tools/audio_channels.py build/runs/<run>/audio.wav             # whole file + 1 s windows
    python tools/audio_channels.py audio.wav --window 0.25 --from 10 --to 18

Prints the RMS of each channel in dBFS (FL FR C LFE SL SR for 6 channels; FL FR C LFE BL BR SL SR
for 8; L R for 2) and, per window, which channels have sound. Distinct channels show up as different
levels (and, in the AUDIO TEST of the settings screen, as one active channel per second). Also
prints the correlation of SL/SR with FL/FR, so a surround pair that is only a copy of the fronts
is visible.
"""
import argparse
import array
import math
import sys
import wave

NAMES = {2: ["L", "R"], 6: ["FL", "FR", "C", "LFE", "SL", "SR"], 8: ["FL", "FR", "C", "LFE", "BL", "BR", "SL", "SR"]}
SILENCE_DB = -60.0


def db(value):
    return 20 * math.log10(value) if value > 0 else -math.inf


def rms(samples):
    return math.sqrt(sum(s * s for s in samples) / len(samples)) / 32768.0 if samples else 0.0


def correlation(a, b):
    sab = sum(x * y for x, y in zip(a, b))
    saa = sum(x * x for x in a)
    sbb = sum(y * y for y in b)
    return sab / math.sqrt(saa * sbb) if saa and sbb else 0.0


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("wav")
    parser.add_argument("--window", type=float, default=1.0, help="seconds per window (default 1)")
    parser.add_argument("--from", dest="start", type=float, default=0.0, help="start, in seconds")
    parser.add_argument("--to", dest="end", type=float, default=None, help="end, in seconds")
    args = parser.parse_args()

    with wave.open(args.wav, "rb") as f:
        channels, rate, width = f.getnchannels(), f.getframerate(), f.getsampwidth()
        if width != 2:
            sys.exit("16-bit PCM only")
        first = int(args.start * rate)
        last = f.getnframes() if args.end is None else min(f.getnframes(), int(args.end * rate))
        f.setpos(min(first, f.getnframes()))
        data = array.array("h", f.readframes(max(0, last - first)))
    if sys.byteorder == "big":
        data.byteswap()
    names = NAMES.get(channels, [f"ch{i}" for i in range(channels)])
    planes = [data[c::channels] for c in range(channels)]
    frames = len(planes[0])
    print(f"{args.wav}: {channels} channels, {rate} Hz, {frames / rate:.1f} s from {args.start:.1f} s")
    print("whole: " + "  ".join(f"{n} {db(rms(p)):6.1f}" for n, p in zip(names, planes)) + "  (dBFS RMS)")
    if channels >= 6:
        surround = (4, 5) if channels == 6 else (6, 7)
        for s, front in zip(surround, (0, 1)):
            print(f"correlation {names[s]}/{names[front]}: {correlation(planes[s], planes[front]):+.2f}")

    step = max(1, int(args.window * rate))
    for start in range(0, frames, step):
        levels = [db(rms(p[start:start + step])) for p in planes]
        active = [n for n, level in zip(names, levels) if level > SILENCE_DB]
        print(f"{args.start + start / rate:7.2f} s  " + " ".join(f"{level:6.1f}" for level in levels) + "   " + (" ".join(active) or "-"))


if __name__ == "__main__":
    main()
