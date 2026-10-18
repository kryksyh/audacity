#!/usr/bin/env python3
"""Run TC3.4 against midi_fixture_macos and compare actual MIDI send times.

Start the compiled fixture with a disposable request directory first. Pass
that directory, an isolated --profile, and an output --log. ASAN_OPTIONS is
inherited, so ASan-specific host settings need not be embedded in this tool.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import time


def main():
    root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app", type=Path, default=root / "build/audacity-asan/src/app/audacity.app/Contents/MacOS/audacity")
    parser.add_argument("--request-dir", type=Path, required=True)
    parser.add_argument("--profile", type=Path, required=True)
    parser.add_argument("--log", type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=120)
    args = parser.parse_args()
    args.profile.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    env.update(CFFIXED_USER_HOME=str(args.profile.resolve()), AU_ALLOW_MULTIPLE_PROCESSES="1",
               MUSE_TESTFLOW_SCRIPTS_PATH=str(root / "share/testflowscripts"),
               MUSE_TESTFLOW_DATA_PATH=str(args.profile.resolve() / "testflow_data"))
    args.log.parent.mkdir(parents=True, exist_ok=True)
    requested, recorded, pending = False, None, ""
    with args.log.open("wb") as output, args.log.open(errors="replace") as reader:
        process = subprocess.Popen([str(args.app), "--test-case", "TC3.4_MidiInputTiming.js", "--test-case-speed", "Fast"],
                                   env=env, stdout=output, stderr=output)
        deadline = time.monotonic() + args.timeout
        try:
            while True:
                finished = process.poll()
                pending += reader.read()
                lines = pending.split("\n")
                pending = lines.pop()
                for line in lines:
                    if not requested and "VIRTUAL MIDI QA READY" in line:
                        phrase = "50 176 64 127\n0 144 65 100\n200 128 65 0\n50 144 67 90\n150 128 67 0\n50 176 64 0\n"
                        (args.request_dir / "request.txt").write_text(phrase)
                        (args.request_dir / "ready").touch()
                        requested = True
                        print("Requested virtual MIDI timing phrase", flush=True)
                    marker = line.find('"Recorded MIDI timings:')
                    if marker >= 0:
                        # Qt logs a quoted JSON string containing the JSON data.
                        decoded, _ = json.JSONDecoder().raw_decode(line[marker:])
                        recorded = json.loads(decoded.removeprefix("Recorded MIDI timings: "))
                if finished is not None:
                    break
                if time.monotonic() >= deadline:
                    raise RuntimeError("Testflow did not finish before its deadline")
                time.sleep(0.005)
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
    if process.returncode != 0:
        raise RuntimeError(f"Testflow failed with exit {process.returncode}; see {args.log}")
    if not requested or recorded is None:
        raise RuntimeError("Testflow did not produce a complete recorded take")
    if (args.request_dir / "done").read_text().strip() != "OK 6":
        raise RuntimeError("Virtual MIDI fixture did not send the complete phrase")
    sent = [line.split() for line in (args.request_dir / "sent.txt").read_text().splitlines()]
    expected_messages = [(176, 64, 127), (144, 65, 100), (128, 65, 0), (144, 67, 90), (128, 67, 0), (176, 64, 0)]
    if [tuple(map(int, row[:3])) for row in sent] != expected_messages:
        raise RuntimeError("Virtual MIDI send log does not match the requested phrase")
    times = [float(row[3]) for row in sent]
    expected = dict(first=times[2] - times[1], second=times[4] - times[3], spacing=times[3] - times[1])
    errors = {key: abs(recorded[key] - expected[key]) for key in expected}
    print(json.dumps(dict(sent=expected, recorded=recorded, error_ms={key: value * 1000 for key, value in errors.items()}), indent=2))
    if max(errors.values()) > 0.003:
        raise RuntimeError("Recorded take differs from actual MIDI send timing by more than 3 ms")
    print("PASS: MIDI timing, velocities, sustain, undo and redo under 700 ms UI load")


if __name__ == "__main__":
    main()
