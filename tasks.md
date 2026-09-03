# OpenTune — Tasks

Ordered and dependency-aware. Work top to bottom, one at a time.
Every task: **failing test first**, then implementation, then review, then commit.

Detail is deliberately front-loaded. Later stages are sketched, and get broken down
properly once the stage before them has taught us what they actually need.

**Legend:** `[ ]` not started · `[~]` in progress · `[x]` done

---

## Stage 0 — Make it audible
*Goal: a WAV goes in, a pitch-shifted WAV comes out. It will sound rough. That is fine —
the point is that every piece exists and is connected.*

### T0.0 — Toolchain `[x]` *(owner, needs sudo)*
`sudo apt install cmake ninja-build clang-format valgrind`, then
`python3 -m venv .venv && .venv/bin/pip install -r requirements-dev.txt`.
**Done when:** `cmake --version`, `clang-format --version`, `valgrind --version` all print,
and `.venv/bin/python -c "import numpy, scipy, matplotlib"` exits 0.
**Note:** Python analysis deps live in a project venv, not system or `~/.local` — a
user-local numpy 2.x was found shadowing apt's 1.x and breaking apt scipy/matplotlib.
They are dev tooling for `tools/analyze.py` only — never a runtime dependency of the engine.
Verified 2026-09-04: cmake 3.28.3, ninja 1.11.1, clang-format 18.1.3, valgrind 3.22.0,
numpy 2.2.6, scipy 1.15.3, matplotlib 3.10.3.

### T0.1 — Project skeleton `[x]`
Create the CMake project: `engine/` library target, `tests/` target, `third_party/`.
C++17, warnings as errors. Add the test framework.
**Depends on:** nothing
**Done when:** `cmake -B build && cmake --build build && ctest --test-dir build` runs
a single trivially passing test.

### T0.2 — Test signal generators `[x]`
`tests/support/Signals.h`: generate sine waves, silence, white noise, and a linear
pitch sweep, at a given frequency, duration, and sample rate.
**Depends on:** T0.1
**Done when:** a generated 440 Hz sine at 48 kHz has verified length, peak amplitude
within 1%, and a zero-crossing count matching 440 Hz within 1%.
**Note:** this is the foundation of every later test — everything downstream measures
against known-correct synthetic input.

### T0.3 — `PitchDetector` interface `[x]`
Write `engine/include/opentune/PitchDetector.h`: `PitchEstimate`, the abstract class,
`prepare` / `reset` / `process`. Header only, no implementation.
**Depends on:** T0.1
**Done when:** it compiles, and a test defines a stub subclass returning a fixed value,
proving the interface is usable.

### T0.4 — Naive autocorrelation detector `[x]`
`AutocorrelationDetector` — find the lag at which the signal best correlates with
itself; that lag is the period, and pitch is its reciprocal. The simplest method that
genuinely works, and the conceptual basis for YIN in Stage 1.
**Depends on:** T0.2, T0.3
**Done when:** detects 110/220/440/880 Hz sines within ±20 cents; reports `voiced=false`
for silence. (±20 cents is deliberately loose — AC1's ±5 cents arrives with YIN.)

### T0.5 — `ScaleQuantizer`, chromatic `[x]`
Frequency → MIDI note → round to nearest → back to frequency.
**Depends on:** T0.1
**Done when:** 440 Hz → 440 Hz (A4 exactly); 445 Hz → 440 Hz; 455 Hz → 466.16 Hz (A#4);
A4 = 440 Hz reference; correct across the full C2–C6 range.
**Corrected 2026-09-04:** this line originally read "452 Hz → 466.16 Hz", which is wrong. The
A4/A#4 decision boundary is MIDI 69.5 = 452.89 Hz, so 452 Hz snaps *down* to 440 Hz.
455 Hz (MIDI 69.58) is the correct example of snapping up to A#4.

### T0.6 — `PitchCorrector` interface `[x]`
Write `engine/include/opentune/PitchCorrector.h`.
**Depends on:** T0.1
**Done when:** compiles; a stub pass-through subclass proves the interface works.

### T0.7 — Naive resampling corrector `[x]`
`ResampleCorrector` — read the input faster or slower with linear interpolation.
This changes pitch *and* duration, which is wrong, and it moves formants, which is the
chipmunk effect. Shipping it deliberately: it makes the pipeline audible today and it
makes the problem Stage 2 solves obvious by ear.
**Depends on:** T0.2, T0.6
**Done when:** a 440 Hz sine at ratio 2.0 produces a 880 Hz sine (±20 cents); ratio 1.0
is bit-identical to input.

### T0.8 — `Params` struct `[x]`
`engine/include/opentune/Params.h` with documented defaults and valid ranges.
**Depends on:** T0.1
**Done when:** compiles; a test asserts defaults match specs.md §7.1.

### T0.9 — `Engine` wiring `[ ]`
Own one of each interface. Per block: detect → quantize → compute ratio → correct.
Unvoiced input passes through untouched (FR2). Strength is not yet applied.
**Depends on:** T0.4, T0.5, T0.7, T0.8
**Done when:** a 445 Hz sine in produces roughly 440 Hz out; silence in produces
silence out unmodified; block boundaries introduce no discontinuity.

### T0.10 — WAV I/O `[ ]`
Vendor `dr_wav` into `third_party/`. Add `tools/autotune-cli/WavFile.h` — mono float
read and write, converting from any input format.
**Depends on:** T0.1 · **Requires approval:** first runtime dependency
**Done when:** a written-then-read WAV round-trips bit-identically.

### T0.11 — CLI tool `[ ]`
`opentune-cli in.wav out.wav [--key C:major] [--strength 0.8]`.
Feeds the file to the engine in 256-sample blocks — the same call pattern a real-time
host will use, so Stage 3 changes only the caller.
**Depends on:** T0.9, T0.10
**Done when:** it processes a real vocal recording end to end and the output is audibly
pitch-shifted. (FR11)

### T0.12 — Analysis tool `[ ]`
`tools/analyze.py in.wav [--out report.png]`: pitch track (Hz and MIDI), RMS envelope,
voiced/unvoiced regions, and a spectrogram, rendered to one PNG. This is how Claude sees
audio — it cannot hear. Also prints a cents-error summary when given `--ref target.wav`.
**Depends on:** T0.0, T0.11
**Done when:** running it on the T0.11 output produces a PNG where the corrected pitch
track visibly snaps to semitone lines, and the numbers match the T0.9 test expectations.

### 🎧 Stage 0 checkpoint
Record yourself singing, run it through the CLI, listen. Expect artifacts — chipmunk
effect, warbling, clicks. **Write down what you hear in `docs/listening-log.md`.**
That list is the agenda for Stages 1 and 2.

---

## Stage 1 — Detect pitch properly
*Goal: meet AC1, AC2, AC7. This is where you learn how pitch detection actually works.*

- [ ] **T1.0** — Build the reference vocal set: 5+ takes (male/female, sung/spoken), pitch hand-labelled per frame. AC2 and AC7 are unmeasurable without it
- [ ] **T1.1** — Test harness measuring detection error in cents across the full range
- [ ] **T1.2** — Octave-error test set (signals with weak or missing fundamentals)
- [ ] **T1.3** — `YinDetector`: difference function
- [ ] **T1.4** — YIN: cumulative mean normalised difference (this is the step that kills octave errors)
- [ ] **T1.5** — YIN: absolute threshold and best-candidate selection
- [ ] **T1.6** — YIN: parabolic interpolation for sub-sample precision (gets us from ±20 to ±5 cents)
- [ ] **T1.7** — Voiced/unvoiced decision from the aperiodicity measure (AC6)
- [ ] **T1.8** — Verify AC1, AC2, AC7 against a labelled vocal set
- [ ] **T1.9** — Benchmark: confirm cost fits the per-block budget

## Stage 2 — Correct pitch properly
*Goal: meet AC3 and pass the listening checklist. The big quality jump.*

- [ ] **T2.1** — Vendor Signalsmith Stretch *(requires approval)*
- [ ] **T2.2** — `SignalsmithCorrector` behind the existing interface
- [ ] **T2.3** — Verify accuracy (AC3) against the naive corrector as a baseline
- [ ] **T2.4** — Enable formant preservation; confirm the chipmunk effect is gone
- [ ] **T2.5** — Latency accounting via `latencySamples()`
- [ ] **T2.6** — A/B listening pass, naive vs Signalsmith, on the Stage 0 recordings
- [ ] **T2.7** — Decision point (Q3): is Signalsmith's formant handling sufficient, or do we need WORLD?

## Stage 3 — Real time
*Goal: meet AC4, AC5, AC9. Sing and hear yourself corrected.*

- [ ] **T3.1** — Vendor miniaudio *(requires approval)*
- [ ] **T3.2** — `opentune-live`: duplex mic-to-speaker passthrough, no processing
- [ ] **T3.3** — Measure baseline round-trip latency with no engine in the path
- [ ] **T3.4** — Insert the engine into the callback (FR12)
- [ ] **T3.5** — Audit the audio path for allocations, locks, and exceptions (constitution II)
- [ ] **T3.6** — Real-time safety test under instrumentation (AC9)
- [ ] **T3.7** — 10-minute soak test, zero dropouts (AC5)
- [ ] **T3.8** — Verify total latency ≤ 20 ms (AC4)

## Stage 4 — The dial
*Goal: G2 and FR7–FR10. One parameter from transparent to hard-tune.*

- [ ] **T4.1** — Lock-free parameter passing from UI thread to audio thread
- [ ] **T4.2** — `strength`: blend between detected and target pitch
- [ ] **T4.3** — `retuneMs`: smooth the pitch trajectory over time
- [ ] **T4.4** — `humanize`: retain controlled deviation so correction is not sterile
- [ ] **T4.5** — Formant preservation toggle
- [ ] **T4.6** — Verify no clicks when parameters change mid-note (FR10)
- [ ] **T4.7** — Listening pass across the full strength range

## Stage 5 — Musical intelligence
- [ ] **T5.1** — Scale types beyond chromatic — major, minor, harmonic minor, pentatonic (FR5)
- [ ] **T5.2** — Key selection API
- [ ] **T5.3** — Pitch-class histogram over a rolling window
- [ ] **T5.4** — Key detection by correlation against key profiles (FR6)
- [ ] **T5.5** — Confidence reporting and manual override

## Stage 6 — Desktop app
- [ ] **T6.1** — Resolve Q5: GUI toolkit, given JUCE is licence-blocked
- [ ] **T6.2** — Window, device selection, level meter
- [ ] **T6.3** — Record, monitor, playback
- [ ] **T6.4** — Parameter controls
- [ ] **T6.5** — WAV export (FR13)

## Stage 7 — Android app
- [ ] **T7.1** — NDK build of the engine
- [ ] **T7.2** — JNI boundary
- [ ] **T7.3** — Resolve Q4: AAudio vs OpenSL ES, measure real device latency
- [ ] **T7.4** — Minimal recording UI (FR14)
- [ ] **T7.5** — Verify AC8 on a mid-range device

---

## Discovered during work
*New tasks found mid-implementation go here, then get slotted into a stage.*

*(empty)*
