"""Convert a PCM WAV file to unsigned 8-bit mono 8 kHz SONG.RAW for Codex Phone."""

import argparse
import array
import wave
from pathlib import Path

TARGET_RATE = 8000


def pcm_value(data: bytes, offset: int, width: int) -> float:
    if width == 1:
        return (data[offset] - 128) / 128.0
    chunk = data[offset : offset + width]
    value = int.from_bytes(chunk, "little", signed=False)
    bits = width * 8
    if value & (1 << (bits - 1)):
        value -= 1 << bits
    return value / float(1 << (bits - 1))


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Make an unsigned 8-bit mono 8 kHz RAW file for the Arduino Mega phone."
    )
    parser.add_argument("input", type=Path, help="input uncompressed PCM WAV")
    parser.add_argument("output", type=Path, nargs="?", default=Path("SONG.RAW"))
    args = parser.parse_args()

    with wave.open(str(args.input), "rb") as source:
        channels = source.getnchannels()
        width = source.getsampwidth()
        rate = source.getframerate()
        frame_count = source.getnframes()
        if source.getcomptype() != "NONE":
            raise SystemExit("Input must be uncompressed PCM WAV.")
        if not 1 <= channels <= 8 or width not in (1, 2, 3, 4) or rate <= 0:
            raise SystemExit("Unsupported WAV format; use PCM WAV with 1–8 channels and 8/16/24/32-bit samples.")
        raw = source.readframes(frame_count)

    frame_width = channels * width
    frames = array.array("f")
    for base in range(0, len(raw) - frame_width + 1, frame_width):
        total = 0.0
        for channel in range(channels):
            total += pcm_value(raw, base + channel * width, width)
        frames.append(total / channels)

    if not frames:
        raise SystemExit("The WAV file contains no audio samples.")

    out_count = max(1, round(len(frames) * TARGET_RATE / rate))
    peak = max(abs(value) for value in frames) or 1.0
    gain = min(0.95 / peak, 8.0)
    output = bytearray(out_count)

    # Linear interpolation gives a compact, dependency-free 8 kHz conversion.
    for index in range(out_count):
        position = index * rate / TARGET_RATE
        left = min(int(position), len(frames) - 1)
        right = min(left + 1, len(frames) - 1)
        fraction = position - left
        sample = frames[left] + (frames[right] - frames[left]) * fraction
        sample = max(-1.0, min(1.0, sample * gain))
        output[index] = max(0, min(255, round(sample * 127 + 128)))

    args.output.write_bytes(output)
    seconds = len(output) / TARGET_RATE
    print(f"Saved {args.output} — {len(output)} bytes, {seconds:.1f} s, mono 8 kHz unsigned 8-bit PCM")


if __name__ == "__main__":
    main()
