# Latency tooling

Tools to measure Audacity latency, and to split each number into a driver/OS part, a PortAudio part and an Audacity part.

## Metrics

| Id | Metric | From → to |
|----|--------|-----------|
| L1 | Output latency | callback buffer → DAC |
| L2 | Input latency | ADC → callback |
| L3 | Monitoring round trip | ADC → app → DAC (software playthrough) |
| L4 | Record alignment | recorded take vs timeline, in samples |
| L5 | Control latency | volume / mute / solo / effect change → audible |
| L6 | Transport latency | Play / Record / Stop → first / last sample |
| L7 | Seek latency | seek during playback → audio from the new position |
| L8 | Display sync | audible sound vs playhead and meters |
| L9 | Plugin delay compensation | alignment of tracks with latent effects |
| L10 | Stability floor | smallest buffer with no dropouts under a fixed load |

Draft targets, compared with reference apps on the same machine, interface, rate and buffer size:

- L1–L3: within 1 buffer of the best reference at 64, 128 and 256 frames, 48 kHz.
- L4: ±1 sample after calibration. Automatic compensation on by default.
- L5: ≤ 1 device buffer + L1.
- L6, L7: ≤ L1 + 20 ms, with no dropout.
- L8: playhead error ≤ 1 display frame. Input meter delay ≤ L2 + 1 frame.
- L9: sample-accurate alignment, also when an effect latency changes during playback.
- L10: no dropouts for 10 minutes at 128 frames with an 8-track project with effects.

## Three layers

```
[A] native     JUCE DemoRunner "AudioLatencyDemo", RTL Utility      driver + OS floor
[B] PortAudio  pa_rtl, linked to the PortAudio that Audacity ships  B - A = PortAudio cost
[C] Audacity   traced app runs (scenarios below)                    C - B = Audacity cost
```

Use the same devices, rate and buffer for all three layers.

## pa_rtl (layer B)

A round-trip harness on PortAudio. It plays a pseudo-random burst once per period and finds it in the input by
cross-correlation. Output and input share one frame counter, so the burst offset is the round trip in samples. It also
prints what `PaStreamInfo` reports, so the reporting error is visible.

```
cmake -S tools/latency/pa_rtl -B build/pa_rtl -G Ninja -DPORTAUDIO_ROOT=$PWD/build/audacity-release/_deps/portaudio
cmake --build build/pa_rtl
build/pa_rtl/pa_rtl --selftest                       # detector check, no audio
build/pa_rtl/pa_rtl --list
build/pa_rtl/pa_rtl --in <dev> --out <dev> --frames 64 [--latency 0.1] [--json]
build/pa_rtl/pa_rtl --host WASAPI --exclusive ...    # Windows
```

`--frames 0 --latency 0.1` copies what Audacity does by default (`paFramesPerBufferUnspecified`, 100 ms).

Use a loopback cable from an output to an input of the same interface. An acoustic path (speaker to microphone) also
works for a first check. It adds about 3 µs per mm of air.

## Traced app scenarios (layer C)

The app records a Perfetto trace (Chrome JSON) while a testflow script drives it:

- the `Audio callback` lane: duration, load, period, output and input peak, PortAudio status flags, ring underruns,
  and the output frame of each onset after a silence;
- zones for the `AudioIO::StartStream` / `StopStream` phases on the GUI thread;
- `SequenceBufferExchange` and the playback queue level on the producer thread;
- a `control: ...` zone in each transport and track-control handler, and a `playhead s` counter.

Open a trace in https://ui.perfetto.dev to see the timeline.

```
tools/latency/run_latency_scenario.sh <label> [AU_LAT_BUFFER_MS=10] [AU_LAT_OUTPUT=<device>]
tools/latency/run_record_alignment.sh <label> AU_LAT_INPUT=<device> [AU_LAT_AUTO_COMPENSATION=0|1] ...
tools/latency/run_pdc_scenario.sh <label> [AU_LAT_BUFFER_MS=5] [AU_LAT_EFFECT=<effect id>]
tools/latency/run_stress.sh <label> [AU_LAT_BUFFER_MS=7] [AU_LAT_TRACKS=8] [AU_LAT_SECONDS=600] [AU_LAT_EFFECTS=<id>,<id>]
```

- `run_latency_scenario.sh` (L5, L6, L7, L8, L10): opens a tone / silence / tone signal. It repeats play, mute, unmute,
  seek into silence, seek into tone and stop. `analyze_trace.py` measures each action to the first callback with the
  new output level, and adds the output latency that PortAudio reports.
- `run_record_alignment.sh` (L4): records the burst train back on a new track through a loopback, exports the tracks
  and runs `pa_rtl --align`. A positive offset means the recording is late on the timeline.
- `run_pdc_scenario.sh` (L9): plays a click at 1.0 s and one at 1.5 s on two tracks, with a latent effect (the built-in
  Compressor by default) on the second. It prints how far the output onsets are from 0.5 s apart after a start, after
  a seek and after the effect is added during play. No loopback needed. Make the signals at the device rate
  (`make_test_signals.py <dir> 44100`): resampling rings before the click, and a louder track crosses the onset level
  earlier.
- `run_stress.sh` (L10): plays 120 s of noise on several tracks with realtime effects on each (Compressor and Reverb
  by default) and prints the engine health counters. The buffer setting is a latency target: PortAudio picks the
  callback size from it, and the health line shows the result (`framesPerBuffer`). The trace keeps only the first
  minute or so of a long run; the health counters cover all of it.

The scripts change the audio preferences of the development profile (buffer, devices, compensation). Write down the
values first and set them back after the runs. They need an explicit `AU_LAT_OUTPUT`, and they stop the app when the
requested device does not exist, so that test signals never play on the system default output. Turn off software
input monitoring (`record.inputMonitoring`) during runs: the app starts monitoring before a scenario configures the
devices, and with a loopback cable that is a feedback loop.

## Procedure per platform

1. Connect the loopback cable. Set the interface to 48 kHz.
2. Layer A: run RTL Utility or the JUCE DemoRunner "AudioLatencyDemo" at 64, 128, 256 and 512 frames.
3. Layer B: run `pa_rtl --frames N --json` for the same sizes, and once with `--frames 0 --latency 0.1`.
   On Windows, run it for each of ASIO, WASAPI (shared and `--exclusive`) and MME.
4. Layer C: run both scenarios with `AU_LAT_BUFFER_MS` set to the same buffer durations.
5. References: in REAPER, Waveform and Ardour, set the same device and buffer. Read the reported round trip, record the
   burst train through the loopback, and measure the take with `pa_rtl --align bursts.wav <exported take>`.
