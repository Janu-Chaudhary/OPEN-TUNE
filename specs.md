# OpenTune — Specification

**Status:** Draft for review · **Version:** v1 · **Last updated:** 2026-09-03

---

## 1. Problem

Real-time vocal pitch correction is locked behind expensive proprietary tools.
Antares Auto-Tune, Waves Tune, and Melodyne are desktop-only, cost hundreds of
dollars, and require a DAW. The free mobile alternatives — Voloco and similar —
reach tens of millions of users but ship 2010-era DSP: correction strength is
hidden behind presets, formants are not preserved, and the result is recognisably
"cheap autotune."

Meanwhile a large population of short-form creators records vocals daily on phones
with no access to real pitch correction at all.

There is no free, open, high-quality pitch correction engine that anyone can build on.

## 2. What we are building

**OpenTune is a portable C++17 real-time pitch correction engine**, plus the
thinnest hosts needed to prove it works and use it.

The engine is the product. The apps are demonstrations of it.

## 3. Goals

- **G1** — Real-time pitch correction with under 20 ms round-trip latency.
- **G2** — One engine spanning transparent correction to hard-tune, controlled by a
  single continuous strength parameter — not two separate modes.
- **G3** — Quality good enough that the output is not obviously worse than a
  commercial tool on the same vocal.
- **G4** — Portable: the same engine compiles unchanged for Linux, Android, and
  later iOS and plugin formats.
- **G5** — Learnable: a contributor with no DSP background can read the code and
  understand what each algorithm does.

## 4. Non-goals for v1

Deferred, not cancelled. Each becomes its own spec later.

| Deferred | Reason |
|---|---|
| Harmony / multi-voice | Built on top of a working corrector; cannot start until it exists |
| Effects (reverb, EQ, compression) | Easy DSP, expensive UI; adds no engine knowledge |
| Beat library / backing tracks | A licensing and hosting problem, not an engine problem |
| Video recording & social export | A different domain entirely; belongs to the app layer |
| iOS app | Requires macOS hardware the owner does not have |
| VST3 / AU plugin | Needs a plugin framework; JUCE is licence-blocked (see constitution I) |
| Offline / lookahead correction mode | Phase 2. Higher quality, but real-time is the differentiator |
| Noise reduction, de-essing, vocal isolation | Adjacent products, not this one |

## 5. Users

**Primary — the short-form creator.** Records vocals on a phone for Reels/Shorts.
Cannot read music, does not know what key they are singing in, will not read a manual.
Needs: press record, sound good, export. Must work with zero configuration.

**Secondary — the developer.** Wants a free pitch correction engine for their own
app, game, or plugin. Needs: clear interfaces, permissive licence, buildable in
under five minutes.

**Tertiary — the learner.** Wants to understand how pitch correction works.
Needs: readable code and honest comments.

## 6. Audio contract

| Property | v1 value | Rationale |
|---|---|---|
| Channels | Mono | A voice is mono; stereo would double CPU for a duplicated signal |
| Sample rate | 48 kHz | Native rate on Android and in pro audio; avoids a resampling stage |
| Sample format | 32-bit float, nominal range ±1.0 | Eliminates an entire class of clipping bug |
| Block size | 256 samples (5.33 ms) | Small enough for the latency budget, large enough for efficient FFTs |
| Latency budget | ≤ 20 ms round trip | Above ~30 ms, live monitoring actively disrupts singing |
| Detectable pitch range | 65 Hz – 1100 Hz (C2–C6) | Covers bass male through high female voice |

**Analysis lag is not output latency.** The detector examines a trailing window of
roughly 2048 samples (~43 ms) of already-received audio. That window costs accuracy
at the very start of a note; it does not delay output. Confusing the two is the most
common way to over-constrain this design.

## 7. Architecture

```
                 ┌──────────────────────────────────────────┐
   mic / file ──▶│                Engine                    │──▶ speaker / file
                 │                                          │
                 │  ┌─────────────┐   frequency (Hz)        │
                 │  │PitchDetector│──────────────┐          │
                 │  └─────────────┘              ▼          │
                 │                        ┌──────────────┐  │
                 │                        │ScaleQuantizer│  │
                 │                        └──────┬───────┘  │
                 │                    target Hz  │          │
                 │  ┌──────────────┐             ▼          │
                 │  │PitchCorrector│◀──── pitch ratio ──────│
                 │  └──────────────┘                        │
                 └──────────────────────────────────────────┘
```

Three interfaces, one job each. Each has a naive implementation (to get audible fast)
and a serious implementation (for quality), swappable without touching anything else.

### 7.1 Files and interfaces

**`engine/include/opentune/PitchDetector.h`**
```cpp
struct PitchEstimate {
    float frequencyHz;   // 0.0f when unvoiced
    float confidence;    // 0.0–1.0
    bool  voiced;        // false for silence, breath, consonants
};

class PitchDetector {
public:
    virtual ~PitchDetector() = default;
    virtual void prepare(double sampleRate, int maxBlockSize) = 0;   // may allocate
    virtual void reset() noexcept = 0;
    virtual PitchEstimate process(const float* block, int n) noexcept = 0;
};
```
Implementations: `AutocorrelationDetector` (naive, Stage 0), `YinDetector` (Stage 1).

**Known octave bias, Stage 0 → Stage 1.** `AutocorrelationDetector` selects the first
qualifying correlation peak rather than the global maximum, so its octave errors lean
**high** (it can lock onto the first harmonic when the fundamental is weak), not low as
textbook autocorrelation would. Stage 1's work against AC7 must start from that fact.
Reasoning: `docs/decisions/0003-first-peak-autocorrelation.md`.

A detector receives 256-sample blocks but analyses a ~2048-sample window, so it keeps an
internal ring buffer. That buffer is **allocated in `prepare()` and never resized**
(constitution II). `process()` returns the most recent estimate available; before the
buffer first fills, it returns `voiced = false`.

**`engine/include/opentune/ScaleQuantizer.h`**
```cpp
enum class ScaleType { Chromatic, Major, NaturalMinor, HarmonicMinor, Pentatonic };
struct Scale { int rootMidiNote; ScaleType type; };

class ScaleQuantizer {
public:
    explicit ScaleQuantizer(Scale scale) noexcept;
    float snap(float frequencyHz) const noexcept;   // nearest allowed pitch, in Hz
    void  setScale(Scale scale) noexcept;
};
```
Pure function of frequency. No state, no allocation, trivially testable.

**`engine/include/opentune/PitchCorrector.h`**
```cpp
class PitchCorrector {
public:
    virtual ~PitchCorrector() = default;
    virtual void prepare(double sampleRate, int maxBlockSize) = 0;
    virtual void reset() noexcept = 0;
    virtual void process(const float* in, float* out, int n,
                         float pitchRatio) noexcept = 0;
    virtual int  latencySamples() const noexcept = 0;
};
```
Implementations: `ResampleCorrector` (naive baseline, Stage 0), `SignalsmithCorrector`
(Stage 0 — pulled forward from Stage 2, see `docs/decisions/0005`).

**`engine/include/opentune/Params.h`**
```cpp
struct Params {
    float strength        = 0.8f;   // 0 = bypass, 1 = full snap (hard-tune)
    float retuneMs        = 40.0f;  // how fast pitch moves to target
    float humanizeCents   = 15.0f;  // random deviation retained, to avoid sterility
    bool  preserveFormants = true;
    Scale scale           = { 0, ScaleType::Chromatic };
};
```
`strength` is the single dial spanning transparent correction to hard-tune (G2).

**`engine/include/opentune/Engine.h`** — owns one of each interface, applies `Params`,
exposes `prepare` / `reset` / `process` / `latencySamples`. Parameters arrive from
other threads via lock-free atomics.

The engine clamps the pitch ratio it computes before handing it to the corrector
(FR15). The clamp is the engine's responsibility, not the corrector's: the corrector
applies the ratio it is told, and should not have to second-guess its arguments.

**Today the clamp never binds**, and the spec should not pretend otherwise. With
chromatic nearest-note snapping the ratio is `snap(f)/f`, bounded to ±50 cents by
construction — measured range 0.9715–1.0293. FR15 guards configurations that do not
exist yet: Stage 4's `retuneMs` smoothing carries a pitch target across blocks, and
Stage 5's gapped scales (pentatonic and harmonic minor have 3-semitone gaps) push
nearest-note snapping to 1.5 semitones, ratio 1.0905. See `docs/decisions/0004`,
which also records the incorrect reasoning FR15 was originally written on.

## 8. Functional requirements

| ID | Requirement |
|---|---|
| FR1 | Engine estimates the fundamental frequency of a mono block, reporting voiced/unvoiced |
| FR2 | Unvoiced input (silence, breath, consonants) passes through uncorrected |
| FR3 | Engine snaps detected pitch to the nearest note of a selected scale |
| FR4 | Chromatic scale requires no user configuration |
| FR5 | User may select a key and scale type manually |
| FR6 | Engine detects the key automatically from audio, with manual override always available |
| FR7 | `strength` continuously blends between no correction and full snap |
| FR8 | `retuneMs` controls how quickly pitch glides to target, preserving scoops and vibrato at higher values |
| FR9 | Formants are preserved when enabled, so correction does not produce the "chipmunk" artifact |
| FR10 | Parameters may change at any time without clicks or dropouts |
| FR11 | CLI processes a WAV file to a WAV file with all parameters settable via flags |
| FR12 | Real-time host captures the microphone and plays corrected audio to the output |
| FR13 | Desktop app allows recording, live monitoring, parameter adjustment, and WAV export |
| FR14 | Android app provides the same core loop on a phone |
| FR15 | Engine clamps the pitch ratio to a musical range before correction (defence-in-depth; does not bind under Stage 0's chromatic snapping) |

## 9. Acceptance criteria

The engine is done when all of the following pass.

### Measured

| ID | Criterion | Threshold |
|---|---|---|
| AC1 | Pitch detection accuracy, synthetic sine 65–1100 Hz | within ±5 cents |
| AC2 | Pitch detection on recorded vocal vs. hand-labelled reference | within ±15 cents, 95% of voiced frames |
| AC3 | Corrected output lands on the target pitch | within ±10 cents |
| AC4 | Round-trip latency, desktop, 256-sample blocks | ≤ 20 ms |
| AC5 | Dropouts during a 10-minute continuous real-time run | zero |
| AC6 | Voiced/unvoiced classification error rate | < 5% of frames |
| AC7 | Octave errors (detecting 2× or ½× true pitch) | < 1% of voiced frames |
| AC8 | CPU usage, one instance, mid-range Android device | < 15% of one core |
| AC9 | Audio-thread allocations, verified under instrumentation | zero |

### Listened

A human listening pass on a fixed set of at least five vocal takes covering male
and female voices, sung and spoken, confirms:

- No warbling or fluttering on sustained notes
- No chipmunk effect on upward correction (formants intact)
- Vibrato survives at `retuneMs` ≥ 60
- Consonants and breaths are not smeared or pitched
- At `strength = 1.0`, the hard-tune effect is clean rather than glitchy
- At `strength = 0.4`, correction is inaudible as an effect

### End-to-end verification

> Sing an untrained, out-of-tune vocal into the desktop app with `strength = 0.7`,
> `retuneMs = 40`, key set to C major. Hear yourself corrected in headphones with no
> perceptible delay. Export the WAV, open it in any analyser, and confirm sung notes
> land within ±10 cents of C-major pitches while breaths and consonants are untouched.

That is the whole product working. If it does not happen, v1 is not done.

## 10. Delivery stages

| Stage | Deliverable | Proves |
|---|---|---|
| 0 | CLI + naive detector + naive corrector | The pipeline is alive and audible |
| 1 | YIN pitch detection | Detection is accurate (AC1, AC2, AC7) |
| 2 | Formants, latency accounting, A/B against the naive baseline | Quality (AC3, listening pass) |
| 3 | Real-time host via miniaudio | Latency and stability (AC4, AC5, AC9) |
| 4 | Full parameter set — strength, retune, humanize | The dial (G2, FR7–FR10) |
| 5 | Key selection, then auto-detection | FR5, FR6 |
| 6 | Desktop GUI app | FR13 |
| 7 | Android app | FR14, AC8 |

Each stage ends in something audible. Stage 0 is measured in days.

## 11. Dependencies

All permissive. All vendored into `third_party/`. Verified 2026-09-03.
Owner-approved: doctest (2026-09-04, see `docs/decisions/0001`), dr_wav (2026-09-04, see
`docs/decisions/0002`), Signalsmith Stretch **and its `signalsmith-linear` dependency**
(2026-09-04, see `docs/decisions/0005`). miniaudio is **not yet approved** — it
needs its own approval at Stage 2 and Stage 3 respectively (constitution I).

| Library | Licence | Used for | Stage |
|---|---|---|---|
| dr_wav | Public domain / MIT-0 | WAV read/write in the CLI | 0 |
| Signalsmith Stretch | MIT (verified from licence text) | Pitch shifting with formant handling | 0 (was 2) |
| signalsmith-linear | MIT (verified from licence text) | STFT, pulled in by Signalsmith Stretch | 0 |
| miniaudio | Public domain / MIT-0 | Cross-platform audio I/O | 3 |
| doctest | MIT | Test framework | 0 | 

Rejected: **Rubber Band** (GPL/commercial), **JUCE** (GPL/commercial), **aubio** (GPL).
Under consideration for Stage 2 if Signalsmith's formant handling proves insufficient:
**WORLD vocoder** (modified BSD; authors state no patents encumber its algorithms).

## 12. Open questions

| # | Question | Needed by |
|---|---|---|
| Q1 | Project licence — permissive, copyleft, or proprietary | Before first public release |
| Q2 | "OpenTune" trademark clearance against Antares "Auto-Tune" | Before any store submission or trademark filing |
| Q3 | Is Signalsmith's formant handling good enough, or is WORLD needed | Stage 2 |
| Q4 | Android audio path — AAudio vs OpenSL ES, and per-device latency reality | Stage 7 |
| Q5 | Desktop GUI toolkit, given JUCE is licence-blocked | Stage 6 |
| Q6 | Commit signing (GPG or SSH) to make authorship provable, not just asserted | Before first public push |
