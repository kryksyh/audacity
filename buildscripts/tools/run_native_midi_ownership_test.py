#!/usr/bin/env python3
"""Run TC3.8 with an owned virtual MIDI source, including real source removal.

Build midi_fixture_macos.cpp first, then pass its executable with --fixture,
an isolated --profile and an output --log. ASAN_OPTIONS is inherited.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile
import time


def stop(process):
    if process is None or process.poll() is not None:
        return
    process.terminate()
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()


def main():
    root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixture", type=Path, required=True)
    parser.add_argument("--profile", type=Path, required=True)
    parser.add_argument("--log", type=Path, required=True)
    parser.add_argument("--app", type=Path, default=root / "build/audacity-asan/src/app/audacity.app/Contents/MacOS/audacity")
    parser.add_argument("--timeout", type=float, default=120)
    args = parser.parse_args()
    args.profile.mkdir(parents=True, exist_ok=True)
    args.log.parent.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    env.update(CFFIXED_USER_HOME=str(args.profile.resolve()), AU_ALLOW_MULTIPLE_PROCESSES="1",
               MUSE_TESTFLOW_SCRIPTS_PATH=str(root / "share/testflowscripts"),
               MUSE_TESTFLOW_DATA_PATH=str(args.profile.resolve() / "testflow_data"))
    source, app = None, None
    requested, unplug_at, removed = set(), None, False
    fixture_log = args.log.with_suffix(".midi.log")
    with tempfile.TemporaryDirectory(prefix="audacity-midi-ownership-") as folder:
        directory = Path(folder)
        with fixture_log.open("wb") as source_output, args.log.open("wb") as output:
            try:
                source = subprocess.Popen([str(args.fixture.resolve()), str(directory)], stdout=source_output, stderr=source_output)
                deadline = time.monotonic() + 5
                while "READY Audacity Native MIDI QA" not in fixture_log.read_text():
                    if source.poll() is not None or time.monotonic() >= deadline:
                        raise RuntimeError("Virtual MIDI source did not become ready")
                    time.sleep(0.01)
                source_id = next(int(line.split()[1]) for line in fixture_log.read_text().splitlines()
                                 if line.startswith("SOURCE_ID "))
                app = subprocess.Popen([str(args.app.resolve()), "--test-case", "TC3.8_MidiInputOwnership.js",
                                        "--test-case-speed", "Fast"], env=env, stdout=output, stderr=output)
                deadline = time.monotonic() + args.timeout
                with args.log.open(errors="replace") as reader:
                    pending = ""
                    while True:
                        finished = app.poll()
                        pending += reader.read()
                        lines = pending.split("\n")
                        pending = lines.pop()
                        for line in lines:
                            if "RECREATE" not in requested and "MIDI OWNERSHIP RECREATE SOURCE" in line:
                                if not removed:
                                    raise RuntimeError("Reconnect requested before original source was removed")
                                source = subprocess.Popen([str(args.fixture.resolve()), str(directory), "--unique-id", str(source_id)],
                                                          stdout=source_output, stderr=source_output)
                                requested.add("RECREATE")
                                print(f"Recreated owned CoreMIDI source with the same ID {source_id}", flush=True)
                            for phase, phrase in (
                                ("OVERLAP", "50 144 60 77\n200 128 60 0\n"),
                                ("CHANNEL_MODE", "50 144 60 77\n200 176 125 0\n"),
                                ("CONTROLLER_RESET", "50 176 64 127\n0 144 67 88\n150 128 67 0\n50 176 121 0\n"),
                                ("UNPLUG", "50 176 64 127\n0 144 60 77\n0 144 67 88\n"),
                                ("RECONNECTED", "50 144 65 91\n200 128 65 0\n"),
                            ):
                                if phase not in requested and f"MIDI OWNERSHIP {phase} READY" in line:
                                    (directory / "done").unlink(missing_ok=True)
                                    (directory / "request.txt").write_text(phrase)
                                    (directory / "ready").touch()
                                    requested.add(phase)
                                    print(f"Requested {phase} phrase", flush=True)
                        if "UNPLUG" in requested and not removed and (directory / "done").exists():
                            if (directory / "done").read_text().strip() != "OK 3":
                                raise RuntimeError("Incomplete unplug phrase")
                            if unplug_at is None:
                                unplug_at = time.monotonic() + 0.2
                            if time.monotonic() >= unplug_at:
                                stop(source)
                                removed = True
                                print("Removed owned CoreMIDI source while keys and pedal were held", flush=True)
                        if finished is not None:
                            break
                        if time.monotonic() >= deadline:
                            raise RuntimeError("Ownership Testflow did not finish before its deadline")
                        time.sleep(0.005)
                if app.returncode != 0 or requested != {"OVERLAP", "CHANNEL_MODE", "CONTROLLER_RESET", "UNPLUG", "RECREATE", "RECONNECTED"} or not removed:
                    raise RuntimeError(f"Ownership Testflow failed: exit={app.returncode}, phases={requested}, removed={removed}; see {args.log}")
                print("PASS: simultaneous keyboard recording, channel-mode release, timed controller reset, device removal/reconnect, sustain release, undo/redo, freeze/unfreeze and save/reopen")
            finally:
                stop(app)
                stop(source)


if __name__ == "__main__":
    main()
