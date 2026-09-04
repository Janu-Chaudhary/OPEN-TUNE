# Synthetic ground-truth voice set — what it is and what it measures

Thirty-three synthesised voice-like signals whose fundamental frequency is
**exact by construction**, together with the per-frame f0 label tracks that
say so. Built by `tools/synth_voices.py`. The `.wav` files are gitignored
(regenerate in seconds); the `.f0.csv` label tracks and this file are tracked.

**This set exists because the real vocal set cannot establish AC2.** See
`docs/decisions/0008`: the T1.0 reference labels are ~10 cents precise and
disagree by more than 15 cents on 25.7% of the frames they agree on, while
AC2's bar *is* ±15 cents. The ruler is the size of the quantity. Here there is
no ruler error at all — the f0 was chosen, not measured.

---

## 1. How the labels can be exact

A voice is a **glottal pulse train** (the source, which carries the pitch)
shaped by **vocal-tract resonances** (the filter, which makes formants and so
vowel identity). This tool builds signals that way round:

1. A phase accumulator advances by `f0[n] / fs` each sample. The per-sample f0
   array **is** the label; nothing estimates it.
2. A Rosenberg (1971) model-B glottal pulse is evaluated on that phase, so the
   pulse shape stays constant while the period follows the trajectory.
3. Lip radiation (a first difference) and a cascade of formant resonators
   (Klatt 1980) shape the spectrum.

Steps 2 and 3 are linear and time-invariant, and an LTI filter **cannot change
the period of a periodic input**. That is the whole reason this works: the
signal gets a voice's spectral structure while f0 stays exactly what was asked
for.

A bare stack of sines would also have exact f0 — and would flatter a detector.
`YinDetector` already measures 0.375 cents on synthetic sines and 83% inside
±15 cents on real singing, so sine accuracy predicts nothing. Source-filter
signals have the harmonic structure, formant peaks and spectral valleys that
actually stress a difference function.

## 2. Provenance of the model values

| Component | Value | Source |
|---|---|---|
| Glottal pulse | Rosenberg model B, OQ 0.56, SQ 2.5 | Rosenberg, *JASA* 49(2B):583–590, 1971 |
| Formant filter | Cascade of 2-pole resonators | Klatt, *JASA* 67(3):971–995, 1980 |
| Formant frequencies F1–F3 | Adult-male vowel means | Peterson & Barney, *JASA* 24(2):175–184, 1952 |
| Formant bandwidths B1–B3 | 140 / 149 / 223 Hz (Childers & Wu 1993) | Xue et al., *J. Commun. Disord.* 74:74–97, 2018 (PMC6002811), Table 5 |
| F4, F5 and B4, B5 | 3400–4700 Hz, 260/300 Hz | Not cited. Generic high formants, present so the spectrum does not run out of resonances above 3 kHz |

**The formants are not Hindi.** The target market is Hindi (`specs.md` §5) and
Hindi vowel formants were wanted. A search did not turn up a table of measured
Hindi vowel formant frequencies that could be quoted with confidence, and
inventing numbers is worse than using documented ones — so these are Peterson &
Barney's American-English adult-male means, mapped onto the nearest Hindi long
monophthong (`aa`→/ɑ/, `ii`→/i/, `uu`→/u/, `ee`→/ɪ/, `oo`→/ʊ/). **This is a
stated limitation, not a claim about Hindi.** It affects vowel *identity* only:
every f0 here is exact whatever formant table shapes it, so no AC1/AC2/AC7
number in section 5 depends on this choice. A sourced Hindi table would change
the timbre and nothing else.

**Why the wide end of the bandwidth range.** Xue et al. report B1 spanning
50–140 Hz across studies. With the narrow end (Fant 1962: 48/50/98 Hz) the F1
resonance is sharp enough that a single harmonic landing on it dominates
everything else by 20+ dB — a spectrum no real recording produces. Measured
against the owner's own takes (median harmonic profile H1…H12 of voiced
150–260 Hz frames in take01/03/05), the narrow set sits 9.6 dB RMS away from
real and Childers & Wu 7.3 dB. **That choice was made against recorded voices
and never against detector output**; tuning a ground-truth generator until the
thing under test passes would destroy the point of having ground truth.

The residual 7.3 dB is a real limitation: the synthetic spectra are still
sparser above H6 than the owner's takes (see section 6).

## 3. The cases

One variable moves per case, so a failure names its own cause.

| Group | Cases | What it isolates |
|---|---|---|
| Steady notes | `steady_0065hz` … `steady_1100hz` (12) | The full 65–1100 Hz declared range (`specs.md` §6), **both endpoints included**. The real set covers only ~93–591 Hz at the percentile level. Vowels rotate so one formant pattern is not the only thing measured. |
| Vibrato | `vibrato_5p5hz_50c`, `vibrato_6hz_100c` | Periodic pitch modulation, 5.5–6 Hz at ±50 and ±100 cents, symmetric in the cents domain |
| Jitter | `jitter_0p5pct`, `jitter_2pct` | Cycle-to-cycle **period** variation. Real folds are not a metronome; 0.2–1% is healthy phonation, 2% is rough |
| Shimmer | `shimmer_5pct`, `shimmer_15pct` | Cycle-to-cycle **amplitude** variation |
| Both | `jitter_shimmer_rough` | 2% jitter and 15% shimmer together — untrained-voice roughness |
| Meend | `meend_350cps` … `meend_3500cps` (5) | Continuous 700-cent glides at 350/700/1500/2400/3500 cents per second. The owner's real takes run a median 345–741 c/s with excursions past 2000 (`testdata/vocals/MANIFEST.md` §5), so the set spans that band and beyond |
| Fundamental | `weak_fundamental`, `missing_fundamental`, `even_harmonics_only`, `telephone_band` | H1 at −20 dB; H1 removed; odd harmonics removed; and a 300–3400 Hz telephone band, which is how a fundamental actually goes missing on a phone. Pins the three points of the corrected `docs/decisions/0003` finding |
| Breathy | `breathy_hnr20db`, `_hnr10db`, `_hnr5db` | Aspiration noise mixed with the pulse train at 20 / 10 / 5 dB harmonics-to-noise ratio |
| Voicing | `voicing_alternating`, `voicing_noisefloor` | Silence / voiced / unvoiced-fricative alternation, over true digital silence and over a −55 dBFS floor. The real set is entirely sung and contains **no** silence, so AC6 and FR2 were previously measured on the easy half of the problem |

`even_harmonics_only` is the one case whose correct answer is **not** f0. An
even-harmonics-only waveform genuinely repeats at P/2, so reporting 2·f0 is
right; only human perception insists on the missing fundamental. It is excluded
from the pooled figures in section 5 and kept as a behaviour pin.

Audio: 48 kHz mono 32-bit float, peak-normalised to 0.7, 1.5 s (steady) or
2.5–3.0 s (everything else). Label tracks use the same geometry as
`testdata/vocals/*.f0.csv` — `time_s` at the centre of a 2048-sample window,
256-sample hop — so `tools/score_detectors.py` consumes both sets unchanged.

### CSV columns

Identical to the real set: `time_s, f0_hz, voiced, consensus, n_agree,
f0_cepstrum_hz, f0_hps_hz, f0_nccf_hz, spread_cents, rms_db`. For a synthetic
case `consensus` is `exact`, `n_agree` is 3, `spread_cents` is 0 and the three
per-estimator columns repeat the exact value — nothing was estimated, so there
is nothing to cross-check. A frame is labelled voiced only if the **entire**
2048-sample window it describes is voiced; windows straddling a voicing
boundary are marked `transition` and carry no label, the same treatment the
real set gives `ambiguous`.

## 4. Verification of the generator itself

A synthesiser with a bug produces confident wrong ground truth, which is worse
than none: it silently redefines "correct" for everything downstream. So the
audio is measured back, by two methods sharing no code with the synthesis path,
and `tools/synth_voices.py` refuses to write a set unless both pass.

**Method A — mean-crossing cycle counting on the glottal source.** The
Rosenberg pulse crosses its own mean once per cycle on the way up. Crossings
are interpolated between samples, counted, and the count is compared against
the number of cycles the label track says fits in that span. Timing wobble
telescopes away over a long span, so a rate error of any size shows up.

| Case group | Cumulative error |
|---|---|
| Steady, 65–1100 Hz (12 cases) | 0.000001 – 0.000091 cents |
| Vibrato (2) | 0.000013 – 0.000140 cents |
| Meend, 350–3500 c/s (5) | 0.000005 – 0.001163 cents |
| Jitter 0.5% / 2% | 0.000046 / 0.000204 cents |
| Shimmer 5% / 15%, jitter+shimmer | 0.019 / 0.058 / 0.080 cents |

The shimmer figures are the *measurement's* limit, not the signal's: amplitude
modulation moves the mean-crossing point within a cycle. Method B measures the
same 15%-shimmer case at −0.00007 cents.

**Method B — long-lag normalised cross-correlation on the final audio**, after
the vocal tract, with no prior on the answer: shortest local correlation
maximum reaching 95% of the best (shortest, not highest — taking the highest is
how you report an octave too low, `docs/decisions/0003`), then a doubling
ladder of lags so that N periods of lag divides the residual timing error by N,
then parabolic interpolation.

| Case group | Error |
|---|---|
| Steady, 65–1100 Hz, all five vowels | 0.000003 – 0.000801 cents |
| Shimmer 15% | 0.00007 cents |
| Breathy, HNR 20 / 10 / 5 dB | 0.002 / 0.007 / 0.013 cents |
| Weak fundamental, missing fundamental | 0.00028 / 0.00025 cents |
| Even harmonics only | **+1200.0001 cents — expected**, the waveform really does repeat at P/2 |

Method B assumes periodicity, so it is not the authority on the jitter cases; a
jittered signal is not periodic and its correlation peak is not defined to a
cent. Method A carries those.

**Worst error anywhere: 0.08 cents, and that one is an artefact of the
measuring method.** The set's labels are good to roughly 1/100 of a cent —
three orders of magnitude tighter than AC2's ±15 cent bar, and about 1000×
tighter than the real set's ~10 cent label spread.

## 5. Measured detector results

`opentune-f0dump` (Release build) over every case, scored by
`tools/score_detectors.py --labeldir testdata/synthetic --cases …`. Full
per-case tables are in the T1.0 synthetic report; the summary:

### AC1 — pitch detection accuracy

| Detector | Worst median error over 12 steady notes, 65–1100 Hz | AC1 bar ±5 c |
|---|---|---|
| `YinDetector` | **0.21 cents** (at 1100 Hz) | **pass** |
| `AutocorrelationDetector` | 1206 cents — two of the twelve are octave errors | fail |

YIN is under 0.25 cents at every one of the twelve notes including both
endpoints. This is the first time AC1 has been checked on voice-like signals
rather than pure sines, and it holds.

### AC2 — within ±15 cents on 95% of voiced frames

**Reported at two time alignments, because the alignment turned out to matter
more than the detector does — see section 7.**

| Detector | at 23.09 ms (scorer default) | at 30.0 ms (measured) |
|---|---|---|
| `YinDetector` | 86.84% | **93.99%** |
| `AutocorrelationDetector` | 83.56% | 87.88% |

11 580 frames, 32 cases, `even_harmonics_only` excluded. **AC2 still fails for
YIN, at 93.99% against a 95% bar** — but it fails narrowly and for two named
reasons, not diffusely. All twelve steady notes, all five meend speeds, both
vibrato cases, shimmer, weak/missing fundamental and the telephone band are at
**100.0%**. What is left is exactly two things:

| Failing case | YIN ≤15 c | Why |
|---|---|---|
| `jitter_2pct` | 54.9% | 2% period jitter is ±35 cents of genuine cycle-to-cycle movement. A 30 ms window averages it; the label does not. Arguably the label is stricter than the criterion is meant to be |
| `jitter_shimmer_rough` | 52.6% | same, 2% jitter |
| `breathy_hnr10db` | 46.5% | **29.0% octave errors** — see AC7 |
| `breathy_hnr5db` | 48.0% | 27.3% octave errors, and recall collapses to 16.4% |

### AC7 — under 1% octave errors

| Detector | at 23.09 ms | at 30.0 ms |
|---|---|---|
| `YinDetector` | 1.37% | **1.34% — fails** |
| `AutocorrelationDetector` | 6.79% | 8.45% |

**Every one of YIN's octave errors comes from the two breathy cases**, and they
are octave **down** (median signed error −1198 cents: it reports 110 Hz for a
220 Hz voice). Excluding `breathy_hnr10db` and `breathy_hnr5db`, YIN's octave
rate over the remaining 30 cases is 0.02%.

This is a specific, actionable diagnosis, it contradicts the natural guess, and
the mechanism was traced rather than assumed. `docs/decisions/0008` established
that YIN's octave errors on the real takes are *flat across pitch velocity* —
meend does not cause them. The synthetic set says what does: **aspiration
noise**, and it is not step 3 that fails but step 3's **fallback**.

`YinDetector` step 3 takes the first lag whose CMND dips below
`kAbsoluteThreshold` = 0.1, which is precisely the rule that makes period
doubling lose. When *no* lag clears 0.1, the code falls back to the **global
minimum** of the CMND — the one selection rule its own comment calls "a coin
flip between P, 2P and 3P". Reimplementing the CMND in numpy over these files
and counting frames:

| Case | frames with no lag under 0.1 | of those, global minimum sitting at 2P |
|---|---|---|
| `breathy_hnr20db` | 0.0% | 0 |
| `breathy_hnr10db` | 46.6% | 13 of 58 frames |
| `breathy_hnr5db` | 100.0% | 22 of 58 frames |

On a sampled frame of `breathy_hnr10db`, CMND(P) = 0.175 and CMND(2P) = 0.126:
neither clears 0.1, the fallback picks 2P, and — the part that matters —
0.126 is comfortably **under** T1.7's `kVoicedAperiodicityMax` of 0.2, so the
frame is published as voiced with an answer an octave low. Tightening that
voicing threshold to 0.126 would not fix this class; the fallback rule itself
is the defect, and a frame that reached the answer by fallback arguably should
not be called voiced at all.

The owner's takes are phone-mic recordings of an untrained voice — exactly the
material where HNR is low.

### AC6 — voiced/unvoiced classification

| Case | YIN | Autocorrelation |
|---|---|---|
| `voicing_alternating` (digital silence) | **0.00% error** (0 false-voiced, 0 false-unvoiced) | 0.00% |
| `voicing_noisefloor` (−55 dBFS floor) | **0.00% error** | 0.00% |

Both detectors classify silence, sustained vowels and fricative noise perfectly
here, against a 5% bar. **This does not settle AC6.** These are clean synthetic
transitions with 8 ms edges; real speech has plosives, breath and voiced
fricatives, and the real set has no speech in it either. AC6 remains measured
only on easy material.

### Autocorrelation's failures, for the record

`AutocorrelationDetector` octave-errors on `steady_0196hz` (91.9%),
`steady_0440hz` (100%) and `vibrato_6hz_100c` (98.0%) — all octave **up**,
exactly the first-qualifying-peak bias `docs/decisions/0003` predicted and
inverted from YIN's. In each case a strong harmonic sits on or near F1. It also
reads −14.4 cents at 988 and 1100 Hz, consistent with the T1.1 baseline.

## 6. What this set is NOT

**(a) It is not a voice.** It is a source-filter model of one. It has no
consonants, no coarticulation, no nasal zeros, no time-varying vocal tract, no
room, no microphone. A detector that passes here has been shown to handle the
*pitch* structure of voiced speech, not speech.

**(b) The spectra are sparser than real ones above H6.** Measured against the
owner's takes, the synthetic median harmonic profile is 7.3 dB RMS away over
H1–H12, with the gap concentrated in the upper harmonics (synthetic ≈ −37 dB at
H8, real ≈ −26 dB). Real voices carry more high-frequency energy than a
five-resonator cascade over a Rosenberg pulse produces.

**(c) The formants are not Hindi** — section 2.

**(d) It is one synthetic speaker.** Nothing here measures speaker
generalisation. It complements the real set, which is one real speaker; neither
answers that question.

**(e) A 100% score is not proof of anything but this set.** Every case is
2.5 seconds of one steady condition. The real takes are 45–96 seconds of a
person singing.

## 7. Finding: the scorer's time alignment is ~7 ms short

This set can do something the real set cannot: because the f0 trajectory is
exact at every sample, the *time* at which a detector's estimate applies can be
measured rather than assumed.

`tools/score_detectors.py` shifts each estimate back by 23.09 ms, derived from
the detectors' buffer geometry (half of 1478 comparison samples plus a 739
sample maximum lag reach). Scanning the offset on the moving cases and taking
the minimum of median absolute error, per case:

| Case | Best offset | median &#124;error&#124; at 23.09 ms → at best |
|---|---|---|
| `meend_350cps` | 30.25 ms | 2.37 → 0.13 cents |
| `meend_700cps` | 30.25 ms | 4.74 → 0.26 cents |
| `meend_1500cps` | 30.25 ms | 9.81 → 0.57 cents |
| `meend_2400cps` | 30.00 ms | 15.35 → 1.03 cents |
| `meend_3500cps` | 29.25 ms | 20.53 → 1.49 cents |
| `vibrato_5p5hz_50c` | 30.00 ms | 7.89 → 1.70 cents |
| `vibrato_6hz_100c` | 30.25 ms | 18.70 → 2.86 cents |

**One offset, 29.25–30.25 ms, across a tenfold range of contour velocity and
two different modulation shapes.** A per-case artefact would not be constant
like that; a fixed group delay is. It also matches the geometry better than
23.09 ms does: YIN's difference function sums over a *fixed* window
`buffer[0..W)` while the lagged copy slides, so the estimate is anchored on
that window, whose centre sits `bufferSize − W/2 = 2218 − 739 = 1479` samples
= **30.81 ms** before the block end — not at the midpoint of the whole buffer.

Consequences, for whoever picks this up:

1. **`DEFAULT_OFFSET_S` in `tools/score_detectors.py` is probably wrong**, and
   is left unchanged here on purpose — correcting it rewrites T1.8's headline
   numbers on the real takes, which is a controller's call, not this task's.
2. **`docs/decisions/0008`'s "velocity wrecks fine accuracy (43% → 11%)" is at
   least partly this artefact.** On exact labels, YIN's median error at
   3500 c/s is 1.49 cents once aligned, and 100% of frames land inside ±15
   cents at every meend speed tested. Window smearing on fast pitch movement is
   far smaller than it looked.
3. The lesson is `docs/lessons.md`'s again, one level up: after checking the
   instrument's resolution, check that it is pointed at the right place.

## 8. Reproducing

```bash
.venv/bin/python tools/synth_voices.py --verify              # generator self-check only
.venv/bin/python tools/synth_voices.py --out testdata/synthetic

cmake -B build-synth -DCMAKE_BUILD_TYPE=Release
cmake --build build-synth --target opentune-f0dump -j
for w in testdata/synthetic/*.wav; do c=$(basename "$w" .wav)
  for d in yin autocorr; do
    ./build-synth/tools/f0-dump/opentune-f0dump "$w" $d > /tmp/synthdump/$c.$d.csv
  done
done
CASES=$(ls testdata/synthetic/*.f0.csv | xargs -n1 basename | sed 's/\.f0\.csv//' | paste -sd,)
.venv/bin/python tools/score_detectors.py /tmp/synthdump \
    --labeldir testdata/synthetic --cases "$CASES"
```

Deterministic: every random draw is seeded, no threshold is fitted to data, and
the generator refuses to write a set that fails its own verification. Runtime
is about 40 seconds for the whole set. Dependencies are numpy and scipy only —
no new dependency was added.
