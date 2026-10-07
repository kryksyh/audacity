#
# Audacity: A Digital Audio Editor
#
# Writes the test signals for the latency runs (48 kHz mono float WAV):
#   control.wav  10 s tone / 10 s silence / 10 s tone. Output peak steps show when
#                play, stop, mute and seek reach the audio callback.
#   bursts.wav   the pa_rtl burst once per second. Record it through a loopback
#                and run align_take.py to measure record alignment (L4).
#
# usage: make_test_signals.py <out dir>
#
import math, os, struct, sys

RATE = 48000
BURST_LENGTH = 512


def write_wav(path, samples):
    data = struct.pack("<%df" % len(samples), *samples)
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVE")
        f.write(b"fmt " + struct.pack("<IHHIIHH", 16, 3, 1, RATE, RATE * 4, 4, 32))
        f.write(b"data" + struct.pack("<I", len(data)) + data)


def burst(gain):
    # Same sequence as tools/latency/pa_rtl/main.cpp makeBurst()
    seed, out = 0x1234567, []
    for _ in range(BURST_LENGTH):
        seed = (seed * 1664525 + 1013904223) & 0xFFFFFFFF
        out.append(gain if seed >> 31 else -gain)
    return out


def main():
    out_dir = sys.argv[1] if len(sys.argv) > 1 else "."
    os.makedirs(out_dir, exist_ok=True)

    tone = [0.5 * math.sin(2 * math.pi * 440 * n / RATE) for n in range(10 * RATE)]
    write_wav(os.path.join(out_dir, "control.wav"), tone + [0.0] * (10 * RATE) + tone)

    b = burst(0.9)
    train = []
    for _ in range(20):
        train += b + [0.0] * (RATE - len(b))
    write_wav(os.path.join(out_dir, "bursts.wav"), train)


if __name__ == "__main__":
    main()
