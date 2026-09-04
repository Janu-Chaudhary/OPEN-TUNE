# Reference vocal set — provenance and measured characterisation

Six solo Hindi vocal recordings by the project owner, plus algorithm-derived
reference f0 labels for each. This file is the deliverable a reader consults;
the audio itself is not in the repository.

**Read the "What these labels are not" section before quoting any number from
this set.**

---

## 1. Provenance

| | |
|---|---|
| Source | The project owner's own recordings. Phone mic, untrained voice, film-song covers. |
| Recorded / converted | 2026-09-04 |
| Content | Solo Hindi vocals. **All six takes are sung.** None is spoken — confirmed by the owner. |
| Format | 48 000 Hz, mono, 32-bit IEEE float WAV (`audioFormat=3`) |
| Licence / redistribution | The owner's private material. **Never committed, never redistributed.** |
| Repository status | `testdata/vocals/*.wav` is gitignored. Only the `.f0.csv` label files and this MANIFEST are tracked. |

Nothing in this set is drawn from a public corpus. That is deliberate: it is
the actual product use case — one untrained voice, one phone, Hindi film
repertoire — and no corpus anywhere contains it.

---

## 2. How the labels were made

`tools/label_f0.py` produces `takeNN.f0.csv` from `takeNN.wav`.

**The labels do not come from OpenTune's detectors, and this is the point.**
Labelling the audio with `YinDetector` and then scoring `YinDetector` against
those labels would measure the detector's agreement with itself and report the
number as accuracy — a detector consistently an octave low would score 100%.

So the labels come from three pitch estimators written from scratch in Python,
chosen because they fail in *different* ways:

| Estimator | Domain | Mechanism | Characteristic failure |
|---|---|---|---|
| Cepstrum | Frequency | Peak in the inverse FFT of the log magnitude spectrum | Confuses the vocal-tract envelope's ripple for the harmonic ripple on low notes; degrades under noise |
| Harmonic Product Spectrum | Frequency | Sum of log magnitudes at `f`, `2f` … `5f` | Octave-**down** slip when a note's own partials are weak; octave-**up** when the fundamental is missing |
| NCCF | Time | Normalised cross-correlation, shortest peak reaching 88% of the best | Follows the waveform, so it breaks on jitter and breathiness rather than on spectral shape |

A frame receives a label only where **at least two of the three agree within
30 cents**. Frames where they do not agree are marked `ambiguous`, carry no f0
value, and are excluded from scoring. **No gap is ever filled with a guess.**

Why 30 cents: it is far below every structural confusion that matters (a fifth
is 702 cents, an octave 1200), while still above the intrinsic precision of the
two spectral methods, below which they disagree on frames where nothing is
actually wrong. The realised precision is better than the tolerance — the
median disagreement inside an agreeing group is 8–12 cents (table 6 below).

The three estimators were validated on synthetic harmonic tones with known f0
before being used. On clean tones from 110 to 880 Hz all three land within
3 cents; the consensus survives each estimator's individual failures (HPS reads
a missing-fundamental 220 Hz tone as 440 Hz and a 3-harmonic tone as 115 Hz, and
the cepstrum misreads an 80 Hz tone entirely — in every case the other two
outvote it), and white noise correctly comes out AMBIGUOUS rather than labelled.

**Voiced/unvoiced** requires both an energy gate and a consensus. The energy
gate is set 30 dB below each take's own 95th-percentile RMS. It is deliberately
referenced to the loud end: these recordings have no silence in them (take04's
RMS spans only 25 dB from its 0.1st to its 99th percentile), so a gate
referenced to the quiet end discards genuinely quiet sung notes.

### CSV columns

`time_s, f0_hz, voiced, consensus, n_agree, f0_cepstrum_hz, f0_hps_hz,
f0_nccf_hz, spread_cents, rms_db`

`time_s` is the centre of the 2048-sample analysis window; the hop is 256
samples (5.33 ms, 187.5 frames/second), matching the engine's nominal block
size. The three per-estimator columns are present so a human can audit any
disagreement without re-running anything.

---

## 3. Coverage and consensus

| Take | Duration | Frames | Voiced | Consensus | 3-way share | Ambiguous |
|---|---|---|---|---|---|---|
| `take01.wav` | 44.7 s | 8377 | 64.4% | 64.4% | 70.5% | 2985 |
| `take02.wav` | 69.4 s | 13001 | 54.8% | 54.8% | 62.5% | 5872 |
| `take03.wav` | 45.5 s | 8525 | 66.3% | 84.0% | 71.9% | 1080 |
| `take04.wav` | 95.8 s | 17965 | 66.7% | 66.7% | 46.5% | 5983 |
| `take05.wav` | 41.5 s | 7777 | 59.4% | 61.8% | 46.4% | 2860 |
| `take06.wav` | 83.2 s | 15589 | 48.6% | 48.6% | 53.7% | 8007 |

*Consensus* is the fraction of energy-gated frames where at least two
estimators agreed. *3-way share* is how many of those agreed frames had all
three estimators agreeing. *Ambiguous* counts the energy-gated frames with no
label.

The disagreement rate is itself a measurement: between 16% and 51% of audible
frames are hard enough that three independent estimators cannot be brought to
within a quartertone of each other. `take03` is the easiest material in the set
and `take06` the hardest.

---

## 4. Measured pitch range

| Take | Median f0 | 5th pct | 95th pct | Range | Min | Max |
|---|---|---|---|---|---|---|
| `take01.wav` | 262.8 Hz | 182.6 Hz | 352.9 Hz | 11.4 st | 67.8 Hz | 936.4 Hz |
| `take02.wav` | 245.7 Hz | 108.7 Hz | 324.4 Hz | 18.9 st | 65.1 Hz | 939.9 Hz |
| `take03.wav` | 194.3 Hz | 142.7 Hz | 242.6 Hz | 9.2 st | 88.6 Hz | 1044.8 Hz |
| `take04.wav` | 298.6 Hz | 213.1 Hz | 591.0 Hz | 17.7 st | 65.8 Hz | 1041.2 Hz |
| `take05.wav` | 165.5 Hz | 108.8 Hz | 217.1 Hz | 12.0 st | 71.8 Hz | 760.6 Hz |
| `take06.wav` | 237.0 Hz | 92.8 Hz | 290.6 Hz | 19.8 st | 65.5 Hz | 1041.4 Hz |

The 5th–95th percentile band is the honest range. The `Min`/`Max` columns are
extremes over individual frames and include the label track's own octave errors
(section 6) — do not read `take06` as spanning 65 Hz to 1041 Hz of real singing.

The set covers roughly 93 Hz to 591 Hz at the percentile level: a middle band of
the engine's declared 65–1100 Hz range. **The extremes of that range are not
exercised by this set.**

---

## 5. Contour shape — what kind of singing this is

All six takes are sung. These measurements say what kind.

| Take | Steady frames | Median slope | 90th pct slope | Fast-motion | Slide events | Longest voiced run |
|---|---|---|---|---|---|---|
| `take01.wav` | 25.9% | 741 c/s | 3012 c/s | 60.9% | 13 | 1.03 s |
| `take02.wav` | 32.4% | 496 c/s | 2389 c/s | 49.7% | 9 | 1.33 s |
| `take03.wav` | 32.8% | 692 c/s | 2410 c/s | 59.7% | 11 | 1.82 s |
| `take04.wav` | 33.1% | 345 c/s | 3538 c/s | 39.6% | 3 | 2.66 s |
| `take05.wav` | 24.2% | 581 c/s | 2270 c/s | 54.9% | 6 | 0.84 s |
| `take06.wav` | 27.9% | 463 c/s | 2300 c/s | 47.3% | 9 | 1.86 s |

*Steady frames* is the share of voiced frames whose surrounding 100 ms stays
within ±50 cents — a sustained-note detector. *Slope* is measured over a 48 ms
baseline, not frame to frame (at a 5.33 ms hop, 3 cents of label wobble is
already 560 c/s, so a per-frame slope measures label noise). *Fast-motion* is
the share of voiced time moving at ≥500 c/s. A *slide event* is ≥500 c/s held
in one direction for ≥150 ms — fast enough to exclude vibrato, which reverses
every ~100 ms.

**What this says.** Only a quarter to a third of voiced time sits on a steady
pitch, and the median contour moves at 350–740 cents/second — the voice is in
motion most of the time it is producing sound. Phrases are short: the longest
unbroken voiced run in any take is 2.66 seconds, and most takes top out near
1–2 seconds. Between 3 and 13 sustained one-directional slides per take are
present, consistent with *meend*-style ornamentation rather than
Western-pop-style held notes.

Ranked by how much sustained material each take offers: `take04` is the most
sustained (33% steady, slowest median slope, 2.66 s phrases, only 3 slide
events) and `take01` the most ornamented (26% steady, fastest median slope,
13 slide events, 1.03 s phrases).

This matters for the engine. A corrector tuned on held notes will be exercised
here mostly on moving pitch, and a detector's window length trades directly
against its ability to track a 3000 c/s transition.

---

## 6. What these labels are NOT

**These are algorithm-derived reference labels. They are not ground truth.**

There is no laryngograph track, no annotator pass, and no hand correction. Three
algorithms cross-checking each other is stronger than one algorithm alone, but
where they share a bias — and they can, since all three ultimately look for
periodicity in the same signal — the consensus is confidently wrong.

The residual error that implies has been measured, not estimated:

| Take | Median label spread | 95th pct spread | Octave inconsistency | Adjacent jump >600 c | Noise floor | p95 level | Gate |
|---|---|---|---|---|---|---|---|
| `take01.wav` | 9.9 c | 26.5 c | 0.36% | 0.33% | -36.0 dBFS | -13.6 dBFS | -43.6 dBFS |
| `take02.wav` | 8.7 c | 25.8 c | 1.79% | 0.60% | -34.7 dBFS | -15.3 dBFS | -45.3 dBFS |
| `take03.wav` | 9.2 c | 25.6 c | 0.37% | 0.29% | -67.2 dBFS | -18.4 dBFS | -48.4 dBFS |
| `take04.wav` | 8.4 c | 25.8 c | 3.43% | 2.14% | -27.7 dBFS | -11.9 dBFS | -41.9 dBFS |
| `take05.wav` | 12.4 c | 27.9 c | 0.63% | 0.10% | -48.0 dBFS | -19.6 dBFS | -49.6 dBFS |
| `take06.wav` | 8.6 c | 25.8 c | 2.49% | 0.45% | -40.7 dBFS | -20.2 dBFS | -50.2 dBFS |

Three named limitations follow.

**(a) Label precision is ~10 cents, not ~1 cent.** The median disagreement
between the estimators that agreed is 8.4–12.4 cents, and the 95th percentile is
about 26 cents. A ±15 cent accuracy figure measured against this set therefore
carries roughly a ±10 cent uncertainty of its own. It is a usable bar but not a
tight one.

**(b) The label track contains octave errors, at 0.36%–3.43% of voiced
frames.** The consensus rule accepts a 2-of-3 vote, so two estimators slipping
the same octave produce a confident wrong label. The figure above is a lower
bound obtained by continuity: a real vocal contour is continuous, so a label
sitting an octave from its own ±0.125 s neighbourhood is almost certainly wrong.
**AC7's bar is <1% octave errors, and on `take02`, `take04` and `take06` the
reference track's own octave error rate exceeds that.** This set cannot resolve
a sub-1% octave-error rate on those takes. `take01`, `take03` and `take05` can.
Restricting scoring to unanimous 3-way frames reduces this contamination, at
the cost of dropping 30–55% of the labels; both figures are reported in the
T1.0 report.

**(c) No spoken material — an AC6 and FR2 coverage gap.** All six takes are
sung, so this set contains no speech at all. AC6 (voiced/unvoiced
classification error under 5% of frames) and FR2 (the unvoiced bypass) are most
stressed by consonants, plosives and breath. Singing has these in far smaller
proportion than speech, because sung vowels are sustained while spoken vowels
are short and constantly interrupted. **Any AC6 figure derived from this set is
measured on the easier half of the problem and will be optimistic.** A spoken
take from the same voice and the same microphone would close the gap; until one
exists, an AC6 number quoted from this set must be quoted with this caveat
attached.

Two further gaps worth naming: the set is **one voice**, so nothing here
measures speaker generalisation; and it exercises only the **middle** of the
declared 65–1100 Hz pitch range.

---

## 7. Reproducing

```bash
.venv/bin/python tools/label_f0.py --stats-json /tmp/stats.json testdata/vocals/take0*.wav
```

Writes `testdata/vocals/takeNN.f0.csv` for each input. Deterministic — no
random seeds, no thresholds fitted to the data. Peak memory is 293 MB and
runtime 49 s for all six takes (the 96-second `take04` alone runs in 11 s);
the tool never builds a whole-file frame array, which is what `tools/analyze.py`
does and why that tool is OOM-killed on this material (tasks.md D8).

The WAV reader parses and honours the `fmt` chunk and raises on any format it
does not handle, rather than assuming 16-bit PCM (tasks.md D9).
