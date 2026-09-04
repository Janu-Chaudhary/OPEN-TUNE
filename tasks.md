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

### T0.9 — `Engine` wiring `[x]`
Own one of each interface. Per block: detect → quantize → compute ratio → correct.
Unvoiced input passes through untouched (FR2). Strength is not yet applied.
The computed ratio is **clamped** to the musical range before it reaches the
corrector (FR15) — see the discovered constraint below.
**Depends on:** T0.4, T0.5, T0.7, T0.8
**Done when:** a 445 Hz sine in produces roughly 440 Hz out; silence in produces
silence out unmodified; block boundaries introduce no discontinuity; a detected
pitch an octave away from the target yields a clamped ratio, not ratio 2.0 or 0.5.

**Discovered constraint — clamp `pitchRatio` (from T0.4 + T0.7 reviews).**
Real correction ratios live in roughly 0.94–1.06; one semitone is 1.0595. Two facts
found during wave 2 combine badly at this seam:
- `ResampleCorrector` starves for input above ratio 1.0 — producing *n* output
  samples needs *ratio × n* input samples, but only *n* arrive per block, so it
  holds its last sample and output becomes block-size dependent.
- `AutocorrelationDetector`'s octave errors lean **high**, not low
  (`docs/decisions/0003`), so a bad frame asks for ratio ≈ 2.0 exactly where the
  corrector is weakest.

Both components passed review individually; the failure exists only where they meet.
T0.9 is the first task that joins them, so the clamp belongs here.

**Done 2026-09-04** (`0e03c97`, fixed in `2b90034`). Review found the implementation sound but
the tests non-discriminating — three of four criteria passed on the null hypothesis. Fixed and
verified by mutation testing:
- Replace the corrector call with a plain copy → **red** (the control assertion
  `|outCents| < |inCents|` fires). Criterion 1 now genuinely defended.
- Delete the unvoiced bypass → **red**. FR2 is proved by white noise with `voiced=false`,
  not by silence, which passes either way.
- Neuter the clamp to identity → **still green**, and that is honest: `snap(f)/f` is bounded
  to ±50 cents, so FR15 cannot bind under Stage 0's chromatic snapping. The code and the
  report both say so plainly rather than implying coverage that does not exist. It becomes
  testable at Stage 4 (cross-block targets) or Stage 5 (gapped scales).

The clamp bound is ±2 semitones (0.8909–1.1225), independently checked against Stage 5's
worst case (1.5 semitones = 1.0905). `clampPitchRatio` lives in an anonymous namespace, not
the public API.

### T0.10 — WAV I/O `[x]`
Vendor `dr_wav` into `third_party/`. Add `tools/autotune-cli/WavFile.h` — mono float
read and write, converting from any input format.
**Depends on:** T0.1 · **Requires approval:** first runtime dependency
**Done when:** a written-then-read WAV round-trips bit-identically.
**Done 2026-09-04** (`c7bb337`). dr_wav v0.14.6 vendored, verified byte-identical to
upstream by the reviewer. `WavFile.h` is host code in `namespace opentune::host`; nothing
under `engine/` references it (constitution IV). Vendored header is a `SYSTEM` include so
strict warnings still apply to our own code — confirmed, not assumed.

### T0.13 — Vendor Signalsmith Stretch `[x]`
`third_party/signalsmith-stretch/` plus its `signalsmith-linear` dependency, both MIT and
both verified from their own licence text. Unmodified, with `LICENSE.txt` and `VENDORED.md`.
**Depends on:** T0.1 · **Approved:** owner, 2026-09-04 (`docs/decisions/0005`)
**Done when:** the header compiles into a test target under the project's strict warning
flags (vendored code as a `SYSTEM` include, as dr_wav is), and a trivial round trip through
it produces audio.

### T0.14 — `SignalsmithCorrector` `[x]`
The real corrector, behind the existing `PitchCorrector` interface. Pulled forward from
Stage 2 (T2.2) because `ResampleCorrector` cannot sustain any ratio ≠ 1.0 — see D5 and
`docs/decisions/0005`. `ResampleCorrector` stays as the naive baseline for A/B work.
**Depends on:** T0.6, T0.13
**Done when:** a 440 Hz sine at ratio 1.0595 (one semitone up) produces 466.16 Hz ±20 cents,
sustained over **at least 10 seconds** of audio — the duration is the point, since that is
exactly what the naive corrector cannot do; block-size invariance holds; and `prepare()`
performs all allocation, with `process()` demonstrated allocation-free rather than asserted
to be (constitution II — the header uses `std::vector`, `std::function` and `std::random`).
**Done 2026-09-04** (`c0c76d6`, fixed `181cb32`, re-review APPROVED). Latency 100 ms after the
preset fix. Zero allocations confirmed by two independent probes. Both upstreams verified
byte-identical to their recorded commits.

### T0.11 — CLI tool `[x]`
`opentune-cli in.wav out.wav [--key C:major] [--strength 0.8]`.
Feeds the file to the engine in 256-sample blocks — the same call pattern a real-time
host will use, so Stage 3 changes only the caller.
**Depends on:** T0.9, T0.10
**Done when:** it processes a real vocal recording end to end and the output is audibly
pitch-shifted. (FR11)
**Done 2026-09-04** (`05890fa`). Mechanically verified by review: exact frame-count
preservation on a length deliberately not a multiple of 256 (10377 in → 10377 out, final
block 137 samples, no padding), output genuinely altered rather than copied, all seven error
paths named and exiting 1, and `--strength 0.2` vs `0.9` byte-identical as `--help` promises.
**The audible half of this criterion is unverified and is the owner's** — the implementer
correctly declined to claim it rather than substituting a mechanical check.

### T0.12 — Analysis tool `[x]`
`tools/analyze.py in.wav [--out report.png]`: pitch track (Hz and MIDI), RMS envelope,
voiced/unvoiced regions, and a spectrogram, rendered to one PNG. This is how Claude sees
audio — it cannot hear. Also prints a cents-error summary when given `--ref target.wav`.
**Depends on:** T0.0, T0.11
**Done when:** running it on the T0.11 output produces a PNG where the corrected pitch
track visibly snaps to semitone lines, and the numbers match the T0.9 test expectations.
**Done 2026-09-04** (`71e015e`). Verified by the controller on a 12 s corrected file: pitch
track pinned to the 440 Hz gridline for the full duration, flat RMS envelope, stable
harmonics in the spectrogram, and a printed cents-error summary. It also earns its keep
immediately — the startup artifact in D6 below was found by looking at its output.

### 🎧 Stage 0 checkpoint
Record yourself singing, run it through the CLI, listen. Expect artifacts — chipmunk
effect, warbling, clicks. **Write down what you hear in `docs/listening-log.md`.**
That list is the agenda for Stages 1 and 2.

---

## Stage 1 — Detect pitch properly
*Goal: meet AC1, AC2, AC7. This is where you learn how pitch detection actually works.*

- [ ] **T1.0** — Build the reference vocal set. **Rewritten 2026-09-04: the original method was
  infeasible.** It said "pitch hand-labelled per frame" — nobody does that. At ~100 frames/second
  a 30-second take is 3000 frames, and no ear resolves ±15 cents per frame on a moving voice.
  Real practice uses a laryngograph, or annotator-corrected algorithmic tracks. Composition
  (owner decision, Hindi-first):
  - **Owner's own Hindi recordings** — phone mic, film-song covers, untrained voice. The actual
    product use case, and in no corpus anywhere. Labels derived algorithmically, owner sanity-checks.
  - **Synthetic source-filter voices** — glottal pulse plus formants, with vibrato, jitter,
    *meend*-style slides and missing fundamentals dialled in one at a time. Exact ground truth by
    construction; the only way to isolate *why* a detector fails rather than *that* it did.
  - **Hindi speech corpora** (IndicVoices-R, Common Voice Hindi) — many speakers, wide pitch
    range. **Licences must be verified before use.**
  - Western research sets (vocadito, PTDB-TUG) are *not* part of this set — owner decision.
  Saraga is local-validation-only (copyrighted audio, see `specs.md` §11).
  **Owner-dependent:** the recordings are yours; AC2 and AC7 stay unmeasurable until they exist.
- [x] **T1.1** — Test harness measuring detection error in cents across the full range
  **Done 2026-09-04** (`1a8f17f`). Baseline established for `AutocorrelationDetector` over
  65–1100 Hz: median 3.58 ¢, mean 4.01 ¢, max 16.35 ¢ — **14 of 49 frequencies (29%) fail AC1's
  ±5 ¢ bar**, worst at 932 Hz (+16.35 ¢). That is the number `YinDetector` must beat.
- [x] **T1.2** — Octave-error test set (signals with weak or missing fundamentals)
  **Done 2026-09-04** (`028a3f3`), and it **disproved decision 0003's prediction.** A missing or
  weak fundamental does *not* induce an octave error — the surviving odd harmonics break
  periodicity at half-period (measured r(P/2) = 0.20 for a removed fundamental, 0.59 for
  H2-dominant). Only an even-harmonics-only signal reaches r(P/2) = 1.0, and that waveform
  genuinely has the shorter period. 0003 and `docs/lessons.md` corrected.
- [x] **T1.3** — `YinDetector`: difference function
- [x] **T1.4** — YIN: cumulative mean normalised difference. **This description was wrong** and
  the implementer disproved it: the running mean plateaus after one period, so P and 2P are
  divided by the same value (1490.69 for a 300 Hz sine) and tie exactly. CMND kills the
  too-*high* family and puts candidates on a common scale so a fixed threshold is possible;
  **T1.5 breaks the octave tie.** See `docs/decisions/0007`
- [x] **T1.5** — YIN: absolute threshold and best-candidate selection — *this is the step that actually resolves P vs 2P*
- [x] **T1.6** — YIN: parabolic interpolation for sub-sample precision. Its RED state was the natural one: without interpolation 11/27 frequencies fall outside ±5 cents, worst +17.40
- [x] **T1.7** — Voiced/unvoiced decision from the aperiodicity measure (AC6). Silence
  aperiodicity 1.0, white noise 0.907–0.929 across 5 seeds, clean sine <0.01; threshold 0.2,
  provisional pending T1.8. A quiet-sine test proves confidence is not a disguised loudness meter
- [~] **T1.8** — Verify AC1, AC2, AC7 against a labelled vocal set. **Split, 2026-09-04:**
  - **AC1 — PASSED.** YIN measures 0.002–0.375 cents across 65–1099 Hz against a ±5 cent bar
    (controller-verified). Autocorrelation reaches −14.4 cents.
  - **AC7 — FAILS, and is actionable.** 8.08% octave errors overall, 14.25% on take04, against
    a <1% bar. Not caused by *meend* — errors are flat across pitch velocity and slightly worse
    on steady notes. The lever is T1.7's voicing threshold, which is far too loose: YIN's own
    confidence already separates good frames from bad (octave error 19.6% below conf 0.95,
    5.4% above). At conf ≥ 0.98 AC7 passes at 0.91% — keeping only 14.6% of frames. That
    trade-off is a listening question, not a numbers one. See `docs/decisions/0008`.
  - **AC2 — MEASURED 2026-09-04 on the synthetic set: 93.99%, fails narrowly** (needs 95%).
    Failure is localised, not general: 100% on all 12 steady notes, all 5 meend speeds *including
    3500 ¢/s*, both vibrato and shimmer cases, weak/missing fundamentals, telephone band, and both
    voicing cases. What remains is 2% jitter and the two breathy cases.
  - **AC7 — root cause found, and it is NOT the voicing threshold.** Every YIN octave error comes
    from breathy phonation and every one is octave *down*; excluding those, 0.02%. Mechanism:
    step 3's **fallback**, which takes the global CMND minimum when no lag clears
    `kAbsoluteThreshold` = 0.1 — a coin flip between P, 2P and 3P by its own comment. Fallback
    fires on 0% of frames at HNR 20 dB, 46.6% at 10 dB, **100% at 5 dB**, and the wrong answer
    passes T1.7's voicing gate comfortably (CMND 0.126 vs a 0.2 gate). **Tightening the threshold
    cannot fix this class.** See `docs/decisions/0008`.
  - **AC2 against the REAL takes — still unmeasurable.** The reference labels are ~10 cents precise (median
    inter-estimator spread 8.37 ¢, p95 25.78 ¢) and disagree by more than 15 cents on 25.7% of
    "agreed" frames. AC2's bar *is* ±15 cents, so the ruler and the quantity are the same size.
    Needs synthetic voices with exact f0, or a laryngograph. **Do not tune the detector against
    this number** — that is fitting to label noise.
- [x] **T1.9** — Benchmark: confirm cost fits the per-block budget.
  **Done 2026-09-04, controller-measured in a Release build** (a Debug build reports 12.7 ms and
  would have failed a check that actually passes). YIN's cost is **constant per call**, ~1.24 ms,
  because the analysis runs over a fixed 1478-sample window with 741 lags regardless of how many
  new samples arrived. So the share of budget scales inversely with block size:

  | Block | Budget | YIN | % of budget |
  |---|---|---|---|
  | 64 | 1.33 ms | 1.262 ms | **94.6%** |
  | 128 | 2.67 ms | 1.272 ms | 47.7% |
  | 256 | 5.33 ms | 1.236 ms | 23.2% |
  | 512 | 10.67 ms | 1.234 ms | 11.6% |

  **At the nominal 256-sample block it fits comfortably. At 64 it does not leave room for a
  corrector.** This bears directly on AC4 and Q7: chasing 20 ms round-trip pushes toward smaller
  host buffers, which is exactly where this cost becomes prohibitive. The lever is decimating the
  analysis rate — run YIN every Nth block and hold the estimate between — not shrinking the
  window, which is already only 2.16 periods at 65 Hz.

## Stage 2 — Correct pitch properly
*Goal: meet AC3 and pass the listening checklist. The big quality jump.*

- [x] **T2.1** — Vendor Signalsmith Stretch — *pulled forward into Stage 0 as T0.13*
- [x] **T2.2** — `SignalsmithCorrector` — *pulled forward into Stage 0 as T0.14*
- [ ] **T2.3** — Verify accuracy (AC3) against the naive corrector as a baseline
- [ ] **T2.4** — Enable formant preservation; confirm the chipmunk effect is gone
- [ ] **T2.5** — Latency accounting via `latencySamples()`
- [ ] **T2.6** — A/B listening pass, naive vs Signalsmith, on the Stage 0 recordings
- [ ] **T2.7** — Decision point (Q3): is Signalsmith's formant handling sufficient, or do we need WORLD?

## Stage 2.5 — Musical intelligence *(promoted from Stage 5, owner decision 2026-09-04)*
*Goal: FR5 and FR6. Promoted because Hindi/Indian film music is the primary market — a
chromatic-only quantizer snaps a Hindi vocal to the wrong notes, so every demo before this
point sounds wrong to the audience the product is for. Task IDs stay `T5.x`: they are
referenced from `specs.md` and the decision records, and renaming them would break those.*

**Scope decision:** Bollywood is harmonium-led and effectively **12-TET**, so the existing
equal-tempered quantizer maths stays valid — raga note-sets sit on top of it. Classical
22-shruti just intonation is **out of scope** (owner decision); it serves trained classical
performers, not this user.

- [ ] **T5.1** — Scale types beyond chromatic. Western major/minor/pentatonic **and raga
  note-sets** for common film-song scales (FR5)
- [ ] **T5.2** — Key/tonic selection API. Sa is movable in Indian music — the tonic goes where
  the singer's voice sits, not to a fixed A440
- [ ] **T5.6** — *(new)* Direction-dependent note sets: many ragas use different notes ascending
  (*aroha*) than descending (*avaroha*). **`ScaleQuantizer::snap()` is currently a pure function
  of frequency and cannot express this** — the correct target depends on where the melody is
  going. This is an interface change, not a lookup table, and it needs a decision record
- [ ] **T5.3** — Pitch-class histogram over a rolling window
- [ ] **T5.4** — Tonic identification by correlation against scale profiles (FR6)
- [ ] **T5.5** — Confidence reporting and manual override

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

- [ ] **D1 — Clamp `pitchRatio` in the engine.** Folded into T0.9 rather than left
  standing alone; kept here as the record of where it came from. Detector octave
  errors (high-biased) feed the corrector's starvation region above ratio 1.0.
  Source: T0.4 and T0.7 reviews, wave 2.
- [ ] **D2 — Fix transitive standard-library includes.** Two independent
  implementations relied on headers they did not include: `<algorithm>` (T0.2) and
  `<cstddef>` (T0.6). One root cause, so one cleanup pass over `engine/` and
  `tests/`, not scattered fixes. Slot: end of Stage 0, with the whole-branch review.
- [ ] **D3 — Resolve the denormal flush-to-zero rule across the engine.**
  `engine/CLAUDE.md` requires it in `prepare()`; neither `ResampleCorrector` nor `Engine`
  applies or explicitly waives it. `Engine` is the natural owner of a process-wide FTZ mode,
  since it is the only class that sees the whole pipeline. Slot: Stage 3, with the real-time
  safety audit (T3.5). Widened from `ResampleCorrector` alone after the T0.9 review.
- [ ] **D4 — Benchmark `AutocorrelationDetector` against the block budget.**
  ~1M multiply-adds per block is constitution-safe but not proven deadline-safe
  against 5.33 ms. Already covered by T1.9; noted here so it is not forgotten.

- [ ] **D5 — `ResampleCorrector` cannot sustain any pitch ratio ≠ 1.0 indefinitely.**
  Above 1.0 it starves: producing *n* output samples needs *ratio × n* input samples and the
  block contract delivers *n*, so the shortfall accumulates. Below 1.0 the read position
  falls behind until it runs off the old end of the bounded 2048-sample history. Either way
  it pins to one sample and every subsequent output block is a single constant value — a
  sample-and-hold at the block rate (187.5 Hz at 48 kHz / 256), which aliases the input down
  to a low buzz with a hard step every 5.33 ms.

  **It is loud, not quiet.** Peak output after collapse is ~0.5 — full input amplitude.
  Measured on the assembled pipeline, first constant-valued block:

  | Input | Direction | Ratio | Collapses at |
  |---|---|---|---|
  | 435 Hz | flat | 1.0115 | 0.68 s |
  | 415 Hz | flat | 1.0243 | 3.38 s |
  | 445 Hz | sharp | 0.9888 | 4.84 s |
  | 466 Hz | — | 1.0003 | not within 6.4 s |
  | 440 Hz | in tune | 1.0000 | never (bit-identical passthrough) |

  **Only an already-in-tune input survives.** Every existing test passes because the longest
  is 0.25 s, not because any direction is safe.

  This is T0.7's deliberate wrongness being worse than anticipated, not a T0.9 defect —
  `ResampleCorrector.h` documents both mechanisms. **Blocks the Stage 0 checkpoint:** the
  owner would be listening for chipmunk artifacts in a file that turns into a buzzing
  staircase after a few seconds.
  **Resolved 2026-09-04:** owner approved pulling Stage 2's corrector forward (T0.13, T0.14,
  `docs/decisions/0005`). `ResampleCorrector` is kept as the naive baseline with this
  limitation pinned by a test, not deleted.
  Source: T0.9 review probe, controller-verified.

- [x] **D6 — Every output file starts with a raw-passthrough burst, then a silent hole.**
  Measured on a 12 s file: samples **0–2047 are bit-identical to the input** (42.7 ms), then
  silence until ~183 ms, then a fade-in to correct output. Two causes compounding, neither a
  bug on its own:
  1. `AutocorrelationDetector` needs ~2048 samples before it can report a pitch, so it
     reports `voiced=false` and FR2's bypass passes the audio through **uncorrected**.
  2. `SignalsmithCorrector` then engages but has 140 ms of latency, so its output is silent
     while its STFT fills.
  Audible as a click and a dropout at the start of every file. Two candidate fixes, neither
  **Worse than first recorded, and fixed 2026-09-04.** Further measurement showed this was
  not only a startup artifact: the bypass emitted with *zero* latency while corrected audio
  was 140 ms late, so **every** voiced/unvoiced boundary was misaligned. On a tone with a
  300 ms silent gap the output gap came out 260 ms long and 40 ms *early*, having swallowed
  140 ms of corrected tail. In singing that is every consonant and every breath.

  Two fixes: `Engine` now routes unvoiced blocks *through* the corrector at ratio 1.0 rather
  than copying, so both paths share one latency whatever corrector is installed; and the CLI
  compensates that latency offline by feeding trailing silence and discarding the leading
  `latencySamples()`. Verified: output length preserved exactly, gap alignment error now
  +29 ms (STFT window smearing, not a timing bug), full amplitude from 0 ms with no hole, and
  correction still holding 440.4 Hz at 0.3 s, 4 s and 11.5 s.

  The cost, recorded honestly: passthrough is no longer bit-identical for a lossy corrector.
  FR2 asks for *uncorrected*, not *bit-identical*, and unity ratio applies no correction.
  Source: controller verification of the T0.11 + T0.14 integration.

- [x] **D7 — `analyze.py` reports silence as a D5 sample-and-hold artifact.**
  Its stuck-frame detector flags any frame where ≥50% of samples repeat the previous one.
  Digital silence satisfies that trivially, so the leading silence from D6 is reported as
  "0.6% of frames are sample-and-hold artifacts -- see tasks.md D5". Silence and a full-scale
  staircase are both constant but are entirely different problems; the check needs an
  amplitude condition so it fires only on the loud case. Small fix, but it matters: this tool
  is how an agent perceives audio, and a false alarm here sends the reader after the wrong bug.
  **Fixed 2026-09-04.** A stuck frame must now also be loud, tested on the *median* absolute
  amplitude rather than the peak — a frame straddling silence and a loud onset is mostly
  repeated zeros *and* contains a loud sample, so a peak test still flagged it. Verified on
  three cases: clean corrected audio 0 flags (was 7), a synthetic D5 staircase still 50%
  flagged with pitch correctly withheld, and pure silence 0% voiced with no alarm.

- [ ] **D8 — `analyze.py` is killed by the OOM killer on files longer than a few seconds.**
  An 89-second WAV exits 137. It was only ever exercised on short synthetic clips, but the
  owner's real takes are 41–96 seconds, which is the normal case for actual material. Needs
  chunked/streamed analysis rather than whole-file arrays. Until fixed, analysis is limited to
  short excerpts, which is a real constraint on measuring AC2 over a full take.

- [ ] **D9 — `analyze.py` and any WAV reader must honour the format tag, not assume 16-bit PCM.**
  The controller read a `audioFormat=3`, 32-bit float WAV as 16-bit int while triaging the
  owner's recordings. That reinterprets each float as two ints: it manufactures broadband noise,
  flattens the RMS envelope, and halves the reported duration — and it produced a confident,
  entirely wrong verdict about the audio ("looks like a dense mix, not solo vocals") that was
  reported to the owner before being caught. `WavFile.h` handles this correctly via dr_wav; the
  Python side and any ad-hoc reader must too. **The tell was six independent recordings all
  reporting RMS 0.5403–0.5414 — identical to four decimal places, which is impossible for real
  audio.** Add a format-tag assertion so this fails loudly instead of silently.

- [x] **D10 — `YinDetector`'s threshold fallback guesses when it should abstain.** *(fixed `5d1b1f7`; the diagnosis below was half right — see the correction at the end of this entry)* When no lag
  clears `kAbsoluteThreshold`, it takes the global CMND minimum. Measured on synthetic breathy
  voices, that is the sole source of YIN's octave errors — 100% of frames fall back at HNR 5 dB,
  and the wrong answer ships as confidently voiced. Options: widen the search before giving up,
  add a cross-frame continuity constraint, or report unvoiced rather than guess. **This is the fix
  for AC7**; the voicing threshold is not. Source: synthetic set, T1.0.
  **Fixed, and the diagnosis was incomplete.** The fallback was only half the mechanism: at
  HNR 10 dB the errors come from step 3 *proper* — `d'(P)` ≈ 0.117 just above the fixed 0.1 while
  `d'(2P)` ≈ 0.088 just below, so first-crossing fires at the octave without ever reaching the
  fallback. Fixing the fallback alone plateaus at 9.19%. What shipped is a *relative* threshold,
  `max(kAbsoluteThreshold, 2.0 × min d')`, which subsumes the fallback entirely.
  **Result: AC7 0.000%, AC2 96.10% — both pass on synthetic.** Real takes 8.08% → 3.19%, still
  above 1%, with take04 alone carrying 82% of the remainder (see D12).
  Abstaining was measured and rejected: it would leave **28.2% of the owner's voiced sung frames
  uncorrected**.

- [x] **D11 — `score_detectors.py`'s analysis-lag offset is wrong.** *(fixed: 23.09 → 30.79 ms)* It assumes 23.09 ms; the
  optimum measured against exact synthetic labels is 29.25–30.25 ms, one constant across a tenfold
  velocity range, and it matches the geometry — YIN sums over `buffer[0..W)` whose centre is
  2218 − 739 = 1479 samples = 30.81 ms back, not the buffer midpoint. Correcting it rewrites
  T1.8's real-take numbers, so it is deliberately unchanged pending that re-run. **Note it does
  NOT explain the velocity gradient** — re-running decision 0008's bins at both offsets moves them
  under 2 points. Source: synthetic set, T1.0.

- [ ] **D12 — real-take AC7 is 3.19%, and take04 carries 82% of it.** D10 took the real set from
  8.08% to 3.19%, a genuine improvement on matched frames (2434 → 1062 octave errors), but not
  under AC7's 1% bar. Re-measured on the corrected D11 offset: take04 10.90%, take02 1.73%,
  take06 1.06%, take03 1.01%, take01 0.55%, take05 0.03% — five of six within touching distance
  of the bar, one an outlier by a factor of six. Three things are
  unestablished and this needs its own task rather than being absorbed into D10: whether take04's
  own labels are octave-correct there (a 2-of-3 consensus of window-based estimators shares the
  period-doubling ambiguity it is being used to judge), whether the synthetic set covers its worst
  passages, and what the residual mechanism actually is.

*Seven further minor review findings are held in `.superpowers/sdd/tasks/progress.md`
for the whole-branch review at the end of Stage 0.*
