#!/usr/bin/env python3
"""Exercise native insert persistence with only our LV2 fixture unavailable.

The project and profile are QA-owned. The fixture bundle is restored even when
a child test fails; no installed/user plugin is moved or reconfigured.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import uuid
from urllib.parse import unquote


ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", type=Path, default=Path("/tmp/audacity-native-testflow-profile"))
    parser.add_argument("--log-prefix", type=Path, default=Path("/tmp/audacity-native-missing-insert"))
    parser.add_argument("--setup-state", type=Path, help="Resume from an actual completed setup phase's state JSON")
    args = parser.parse_args()
    profile = args.profile.resolve()
    if not profile.is_relative_to(Path("/tmp").resolve()) or not profile.name.startswith("audacity-native-"):
        parser.error("Use a dedicated /tmp/audacity-native-* QA profile")
    app = ROOT / "build/audacity-asan/src/app/audacity.app/Contents/MacOS/audacity"
    fixture = ROOT / "build/audacity-asan/src/au3audio/tests/lv2plugin/native-instrument.lv2/native_lv2_fixture.so"
    if not app.is_file() or not fixture.is_file():
        parser.error("Build the native macOS ASan app and native_lv2_fixture first")
    original_hash = hashlib.sha256(fixture.read_bytes()).hexdigest()
    print("Owned fixture binary SHA256", original_hash, flush=True)
    bundle = fixture.parent
    # Lilv scans all direct children of LV2_PATH, irrespective of suffix. Move
    # the bundle outside that directory, including its cached port metadata.
    backup = bundle.parent.parent / (".missing-test-" + uuid.uuid4().hex + "-" + bundle.name)
    plugin_path = "urn:audacity:test:fixed-audio-effect@" + fixture.parent.as_uri() + "/"
    catalog = profile / "Library/Application Support/Audacity/Audacity4Development/known_audio_plugins.json"
    env = os.environ.copy()
    env.update(
        CFFIXED_USER_HOME=str(profile), AU_ALLOW_MULTIPLE_PROCESSES="1",
        MUSE_TESTFLOW_SCRIPTS_PATH=str(ROOT / "share/testflowscripts"),
        MUSE_TESTFLOW_DATA_PATH=str(profile / "testflow_data"),
        LV2_PATH=str(fixture.parent.parent),
        ASAN_OPTIONS="symbolize=1:abort_on_error=1:external_symbolizer_path=/opt/homebrew/opt/llvm/bin/llvm-symbolizer:"
                     "suppressions=/tmp/audacity-tracktion-macos-image-registration-asan.supp",
    )
    children = set()

    def restore_fixture():
        if not backup.exists():
            return
        if bundle.exists():
            raise RuntimeError(f"Concurrent fixture replacement; retained original at {backup}")
        backup.rename(bundle)
        print("Restored owned fixture bundle", flush=True)

    def interrupted(signum, frame):
        raise KeyboardInterrupt

    signal.signal(signal.SIGTERM, interrupted)

    def run(arguments, log, pin=False):
        child_env = env.copy()
        # Pin only this owned ASan fixture while it is available. Keeping its
        # image mapped avoids Darwin ASan's known dlclose/redzone reuse issue.
        if pin:
            child_env["DYLD_INSERT_LIBRARIES"] = str(fixture)
        else:
            child_env.pop("DYLD_INSERT_LIBRARIES", None)
        log.parent.mkdir(parents=True, exist_ok=True)
        with log.open("wb") as output:
            child = subprocess.Popen([str(app)] + arguments, cwd=ROOT, env=child_env,
                                     stdin=subprocess.DEVNULL, stdout=output, stderr=output)
            children.add(child)
            print("Started", child.pid, log, flush=True)
            try:
                result = child.wait(timeout=180)
            finally:
                if child.poll() is None:
                    child.terminate()
                    try:
                        child.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        child.kill()
                        child.wait()
                children.discard(child)
            if result:
                raise RuntimeError(f"Child exited {result}; see {log}")

    def validate(suffix):
        output = Path(str(args.log_prefix) + f"-{suffix}-validation.json")
        run(["--register-audio-plugin", plugin_path, "--register-audio-plugin-out", str(output)],
            Path(str(args.log_prefix) + f"-{suffix}-validation.log"), pin=True)
        results = json.loads(output.read_text())["plugins"]
        if len(results) != 1 or results[0]["path"] != plugin_path or results[0]["state"] != "Validated":
            raise RuntimeError(f"Fixture validation failed: {output}")
        # Apply the verbatim actual validator result to the matching entry of
        # this QA catalog only, with all child applications stopped.
        data = json.loads(catalog.read_text())
        original = catalog.read_bytes()
        matches = [i for i, item in enumerate(data["plugins"]) if item["path"] == plugin_path]
        if len(matches) > 1:
            raise RuntimeError("Ambiguous QA catalog fixture entry")
        if matches:
            data["plugins"][matches[0]] = results[0]
        else:
            data["plugins"].append(results[0])
        Path(str(args.log_prefix) + f"-{suffix}-catalog-before.json").write_bytes(original)
        catalog.write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n")

    def phase(name, state=None):
        log = Path(str(args.log_prefix) + f"-{name}.log")
        options = {"phase": name}
        if state is not None:
            options["state"] = state
        run(["--test-case", "TC3.16_MissingInsert.js", "--test-case-speed", "Fast",
             "--test-case-func-args", json.dumps([options])], log, pin=name != "missing")
        text = log.read_text(errors="replace")
        if "[testflow] PASSED status=Finished" not in text:
            raise RuntimeError(f"Missing completed Testflow result: {log}")
        matches = re.findall(r'MISSING_EFFECT_STATE=([^"\s]+)', text)
        if len(matches) != 1:
            raise RuntimeError(f"Missing unambiguous phase state: {log}")
        state = json.loads(unquote(matches[0]))
        if not Path(state["path"]).resolve().is_relative_to(profile):
            raise RuntimeError("Test project escaped the dedicated profile")
        Path(str(args.log_prefix) + f"-{name}-state.json").write_text(json.dumps(state, indent=2) + "\n")
        print("Phase", name, "PASS", state["path"], flush=True)
        return state

    try:
        validate("before")
        if args.setup_state:
            state = json.loads(args.setup_state.read_text())
            if not Path(state["path"]).resolve().is_relative_to(profile):
                raise RuntimeError("Setup project escaped the dedicated profile")
            print("Reusing completed setup", args.setup_state, flush=True)
        else:
            state = phase("setup")
        bundle.rename(backup)
        try:
            state = phase("missing", state)
        finally:
            restore_fixture()
        if hashlib.sha256(fixture.read_bytes()).hexdigest() != original_hash:
            raise RuntimeError("Fixture binary changed during the test")
        print("Verified restored fixture SHA256", original_hash, flush=True)
        validate("restored")
        phase("restored", state)
        print("Native missing-effect workflow PASS", flush=True)
    finally:
        for child in tuple(children):
            child.terminate()
            child.wait(timeout=5)
        restore_fixture()


if __name__ == "__main__":
    try:
        main()
    except (Exception, KeyboardInterrupt) as error:
        print("Native missing-effect workflow failed:", error, file=sys.stderr)
        sys.exit(1)
