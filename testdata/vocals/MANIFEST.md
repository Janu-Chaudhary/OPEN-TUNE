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

---

## 8. Third-party reference set — provenance

Nine recordings the project owner already had locally, not the owner's own
performance. **These fall under the same policy `specs.md` §11 already records
for Saraga: local validation only.** The recordings are third-party,
copyrighted material — film-song covers and one lecture recording, sourced
from YouTube-style uploads the owner downloaded before this task. **The audio
is never committed, never redistributed, never shipped in any build.** Only
the derived `.f0.csv` label tracks and this manifest text are tracked, exactly
as for the owner's own `takeNN.wav` set above — those are text, and they are
what any measurement cites.

| | |
|---|---|
| Source | Nine third-party recordings, owner's local Downloads folder, decoded 2026-09-04 |
| Decode | `gst-launch-1.0` (`decodebin ! audioconvert ! audioresample ! rate=48000,channels=1 ! wavenc`) |
| Format | 48 000 Hz, mono, 16-bit PCM WAV (`audioFormat=1`) — the GStreamer default, **not** the 32-bit float of the owner's own takes. `label_f0.py`'s fmt-chunk-honest reader (tasks.md D9) handles both; nothing here required a code change. |
| Content | Eight solo/near-solo Hindi vocal recordings (film-song covers, "vocals only" uploads) plus one spoken Hindi engineering lecture |
| Licence / redistribution | Third-party copyrighted. **Never committed, never redistributed, never shipped.** Same handling as Saraga (`specs.md` §11). |
| Repository status | `testdata/vocals/ref*.wav` is gitignored by the existing `testdata/vocals/*.wav` pattern. Only `ref*.f0.csv` and this manifest are tracked. |

**Naming.** `refNN_<performer-or-source>_<title-fragment>.wav`, numbered to sort
after the owner's `take01`–`take06`. `ref01`–`ref08` are sung; `ref09` is
spoken and named accordingly so a reader does not have to open it to find out.

| File | Performer | Description |
|---|---|---|
| `ref01_atif_jeena_jeena.wav` | Atif Aslam (film playback) | Male, professional trained voice — "Jeena Jeena" (*Badlapur*), vocals-only upload |
| `ref02_navjot_khat.wav` | Navjot Ahuja | Male, the lowest-reaching sung material in this set |
| `ref03_abuhasan_hindi.wav` | uncredited (channel: Abu Hasan babu) | Female, untitled Hindi vocals-only cover |
| `ref04_simran_tuthodidereh.wav` | Simran Kaur | Female, upper range — "Tu Thodi Der" (Shreya Ghoshal cover) |
| `ref05_vidhisha_maintenusamjawan.wav` | Vidhisha Vishwas | Female, upper range — "Main Tenu Samjawan" unplugged cover |
| `ref06_seerat_saiyaara.wav` | Seerat Ain Alam | Female, raw/untrained — "Saiyaara" female-version cover, phone-voice-note style |
| `ref07_seerat_khat.wav` | Seerat Ain Alam | Female, raw/untrained — same singer as `ref06`, "Khat" cover |
| `ref08_shabnam_dilcheezhaikya.wav` | Shabnam Majeed | Female, upper range — "Dil Cheez Hai Kya", **accompaniment suspicion, see §12.2** |
| `ref09_lecture_hindi.wav` | uncredited lecturer (channel: Last moment tuitions) | Spoken Hindi engineering-chemistry lecture. **First 60 s cut, see §12.1.** The only spoken material in the whole reference set (owner's + third-party). |

Labelled with the identical `tools/label_f0.py` consensus method described in
§2 above — same three estimators, same 30-cent tolerance, same energy gate,
same `WINDOW=2048`/`HOP=256`. No parameter was changed for this set.

---

## 9. Coverage and consensus (third-party set)

| File | Duration | Frames | Voiced | Consensus | 3-way share | Ambiguous |
|---|---|---|---|---|---|---|
| `ref01_atif_jeena_jeena.wav` | 147.0 s | 27560 | 67.6% | 83.6% | 72.0% | 3644 |
| `ref02_navjot_khat.wav` | 263.0 s | 49313 | 72.8% | 86.5% | 70.8% | 5604 |
| `ref03_abuhasan_hindi.wav` | 63.7 s | 11931 | 79.8% | 86.9% | 83.9% | 1433 |
| `ref04_simran_tuthodidereh.wav` | 176.1 s | 33005 | 80.5% | 81.9% | 76.3% | 5867 |
| `ref05_vidhisha_maintenusamjawan.wav` | 60.1 s | 11265 | 69.5% | 89.4% | 82.3% | 927 |
| `ref06_seerat_saiyaara.wav` | 55.5 s | 10401 | 78.5% | 87.9% | 80.4% | 1126 |
| `ref07_seerat_khat.wav` | 43.6 s | 8169 | 75.4% | 84.4% | 83.3% | 1142 |
| `ref08_shabnam_dilcheezhaikya.wav` | 162.5 s | 30462 | 78.8% | 83.9% | 58.0% | 4606 |
| `ref09_lecture_hindi.wav` (post-cut) | 818.2 s | 153411 | 36.6% | 41.4% | 41.4% | 79643 |

Columns match §3 above. **`ref09`'s numbers are the clearest confirmation
that it is speech, independent of anyone's label of the source file**: voiced
fraction (36.6%) and consensus rate (41.4%) are both roughly half the sung
takes' figures, because speech interleaves short voiced vowels with
consonants, plosives and silence far more than sustained singing does — see
§13 and the existing AC6 caveat in §6(c). Its 3-way agreement share (41.4%) is
also the lowest in either set, i.e. the hardest material yet labelled: a fast,
low-amplitude, low-sustain signal is harder for three independent estimators
to agree on than a held sung note. `ref08` is the weakest of the eight sung
takes by this same measure (58.0% 3-way share, next-lowest is `ref01` at
72.0%) — one of the signals behind the accompaniment check in §12.2.

---

## 10. Measured pitch range (third-party set)

| File | Median f0 | 5th pct | 95th pct | Range | Min | Max |
|---|---|---|---|---|---|---|
| `ref01_atif_jeena_jeena.wav` | 212.9 Hz | 165.1 Hz | 293.6 Hz | 10.0 st | 72.7 Hz | 1021.6 Hz |
| `ref02_navjot_khat.wav` | 245.3 Hz | 165.0 Hz | 329.9 Hz | 12.0 st | 68.4 Hz | 977.7 Hz |
| `ref03_abuhasan_hindi.wav` | 299.0 Hz | 216.3 Hz | 411.4 Hz | 11.1 st | 125.2 Hz | 892.9 Hz |
| `ref04_simran_tuthodidereh.wav` | 397.8 Hz | 335.3 Hz | 526.8 Hz | 7.8 st | 70.7 Hz | 916.3 Hz |
| `ref05_vidhisha_maintenusamjawan.wav` | 434.3 Hz | 264.6 Hz | 534.9 Hz | 12.2 st | 173.8 Hz | 1092.9 Hz |
| `ref06_seerat_saiyaara.wav` | 369.7 Hz | 285.5 Hz | 492.8 Hz | 9.5 st | 171.7 Hz | 1085.0 Hz |
| `ref07_seerat_khat.wav` | 340.4 Hz | 263.6 Hz | 436.7 Hz | 8.7 st | 238.4 Hz | 1106.9 Hz |
| `ref08_shabnam_dilcheezhaikya.wav` | 414.8 Hz | 318.1 Hz | 518.0 Hz | 8.4 st | 95.4 Hz | 1088.8 Hz |
| `ref09_lecture_hindi.wav` | 211.9 Hz | 142.1 Hz | 277.4 Hz | 11.6 st | 65.1 Hz | 1094.5 Hz |

As in §4: the `Min`/`Max` columns are per-frame extremes and include the label
track's own octave errors (§11 below); the 5th–95th percentile band is the
honest range. At the percentile level this set covers roughly **142 Hz to
535 Hz** — noticeably higher-pitched on average than the owner's own six takes
(93–591 Hz band, median), because six of the eight singers here are female and
several are singing in an upper tessitura by description. `ref02` and `ref09`
anchor the low end; `ref05` and `ref07`'s p95 anchor the high end.

---

## 11. Residual label error (third-party set)

| File | Median spread | 95th pct spread | Octave inconsistency | Adjacent jump >600 c |
|---|---|---|---|---|
| `ref01_atif_jeena_jeena.wav` | 7.3 c | 25.9 c | 0.15% | 0.08% |
| `ref02_navjot_khat.wav` | 6.1 c | 24.8 c | 0.80% | 0.22% |
| `ref03_abuhasan_hindi.wav` | 7.9 c | 25.3 c | 0.12% | 0.01% |
| `ref04_simran_tuthodidereh.wav` | 7.6 c | 26.0 c | 0.41% | 0.10% |
| `ref05_vidhisha_maintenusamjawan.wav` | 7.6 c | 25.2 c | 0.06% | 0.03% |
| `ref06_seerat_saiyaara.wav` | 8.7 c | 26.3 c | 0.04% | 0.00% |
| `ref07_seerat_khat.wav` | 8.3 c | 25.0 c | 0.03% | 0.00% |
| `ref08_shabnam_dilcheezhaikya.wav` | 8.9 c | 26.2 c | **0.76%** | **0.46%** |
| `ref09_lecture_hindi.wav` | 12.4 c | 27.7 c | 1.63% | 0.99% |

Same method as §6(b): a lower bound on the label track's own error rate,
obtained by continuity. `ref08` has the second-highest octave-inconsistency
and adjacent-jump rate of the eight sung takes (after `ref02`, whose low
range is the more usual cause — a low fundamental leaves fewer harmonics
inside the analysis band, which is exactly the failure mode HPS and the
cepstrum are prone to). `ref08` has no such excuse: its median f0 (415 Hz) is
mid-set, not low. This is one of the two independent signals feeding the
accompaniment check in §12.2 (the other being its unusually low 3-way
agreement share, §9). **Neither signal proves accompaniment by itself — both
are also consistent with reverb, sibilance, or simply a harder recording —
but together they are enough to flag the file rather than wave it through.**

`ref09` (speech) has the highest spread and inconsistency of anything labelled
so far, consistent with speech's much shorter, more rapidly-changing voiced
segments (§13) giving each estimator less signal per frame to lock onto.

---

## 12. Two findings from re-checking this set

### 12.1 `ref09_lecture_hindi.wav` — music intro cut

The source video's first ~15 s carries a bass-heavy sting/jingle, peaking
around 5–10 s roughly 24 dB above the surrounding speech floor and settling
back down by ~15 s. Analysing this stretch would measure the jingle, not the
speaker. **The first 60 seconds of the decoded audio were cut before writing
`ref09_lecture_hindi.wav`** (60 s is a deliberately generous margin past the
~15 s settling point). The source file is 14.6 minutes; after the cut,
`ref09_lecture_hindi.wav` is **13.64 minutes (818.2 s)** — confirmed by the
WAV header (`duration_s=818.23` in the labeller's own output) — and by far the
longest file either reference set has labelled. `tools/label_f0.py` handled it
without incident: see §12.3.

### 12.2 `ref08_shabnam_dilcheezhaikya.wav` — accompaniment/reverb re-check

**The controller's measurement:** a gap/loud RMS ratio of 0.092, against
0.006–0.051 for every other file in this set — high enough to suggest residual
accompaniment or a reverb tail surviving under the phrases, since a clean
solo-vocal recording should have a much quieter gap than a passage with
something (even quietly) sounding underneath it.

**My re-check disagrees with that number.** Using the same 5th/95th-percentile
frame-RMS metric `label_f0.py` itself computes for its voicing gate (2048-sample
window, 256-sample hop, exactly `noise_floor_db`/`loud_p95_db` in the tool's
own JSON output — i.e. this is not a different instrument, it is the one this
whole pipeline already trusts), the ratio for every file in this set is:

| File | Noise floor | Loud (p95) | Gap/loud ratio |
|---|---|---|---|
| `ref01_atif_jeena_jeena.wav` | −61.1 dBFS | −13.6 dBFS | 0.0042 |
| `ref02_navjot_khat.wav` | −90.8 dBFS | −15.4 dBFS | 0.0002 |
| `ref03_abuhasan_hindi.wav` | −51.1 dBFS | −12.8 dBFS | 0.0122 |
| `ref04_simran_tuthodidereh.wav` | −43.6 dBFS | −15.9 dBFS | 0.0412 |
| `ref05_vidhisha_maintenusamjawan.wav` | −71.3 dBFS | −19.6 dBFS | 0.0026 |
| `ref06_seerat_saiyaara.wav` | −56.6 dBFS | −18.9 dBFS | 0.0130 |
| `ref07_seerat_khat.wav` | −74.8 dBFS | −18.3 dBFS | 0.0015 |
| `ref08_shabnam_dilcheezhaikya.wav` | −51.5 dBFS | −17.3 dBFS | **0.0195** |
| `ref09_lecture_hindi.wav` | −51.3 dBFS | −14.2 dBFS | 0.0140 |

By this measurement `ref08` sits mid-pack — higher than five of the other
eight files, but well inside their spread, and lower than `ref04`'s 0.0412.
It is not an outlier here. I tried several variants of the metric (20 ms and
100 ms frames, 1st/99th and 5th/95th percentile pairs, on the raw decoded
audio rather than the labeller's own frame grid) and none reproduced anything
near 0.092 for `ref08`; all landed in the same 0.01–0.03 neighbourhood. I
cannot explain the gap between the controller's 0.092 and my ≤0.041 across
every method tried — possibly a different frame size, a different channel
(stereo left/right before downmix), or a different segment of the file.

**What I can say, and it does still point toward `ref08` being the weakest
file in this set:** independent of the RMS-ratio disagreement, `ref08` has
the lowest 3-way estimator agreement of the eight sung takes (§9, 58.0% vs.
72–89% for the rest) and the second-highest octave-inconsistency and
adjacent-jump rates (§11), with no low-pitch excuse for either (its median f0
is mid-set). That is two independent, unrelated signals both landing on the
same file. **I did not confirm accompaniment or a reverb tail by RMS ratio,
but I also did not clear the file.** I cannot listen to it (CLAUDE.md). The
honest state to record is: RMS-ratio evidence is inconclusive/contradicts the
controller's figure; label-quality evidence (agreement share, octave
inconsistency) independently flags this as the shakiest of the eight sung
takes. **Anyone using `ref08` for detector scoring should treat it as a
hazard file pending an owner listening check**, exactly as the brief that
commissioned this set required — a solo-vocal assumption a monophonic
detector depends on is not something a label-agreement statistic alone can
clear.

### 12.3 `tools/label_f0.py` on 13.6 minutes of audio

Untested until now: every previously labelled file (owner's six takes, T1.0's
synthetic set) is under 100 seconds. `ref09_lecture_hindi.wav` at 818 s is
**8.5x longer than the longest file the labeller had processed before
(`take04`, 95.8 s).** It completed without incident: exit code 0, all nine
files (this one included) processed in a single invocation in **4 m 46 s**
wall time (4 m 6 s user CPU), peak process RSS observed around 275 MB during
the run — in the same range as the tool's documented 293 MB peak for the
owner's whole six-take set (§7), because `CHUNK_FRAMES` bounds memory
independent of file length, exactly as the tool's own memory design intends.
**No workaround, truncation, or parameter change was needed.** For scale: the
labeller costs roughly 4–5 minutes of CPU for ~30 minutes of combined audio in
this run, and the 13.6-minute lecture alone dominates that cost — a useful
number for judging whether a future run has hung rather than merely being
slow on a long file.

---

## 13. Coverage — third-party set folded into the whole reference corpus

Read together with §4's owner-recorded coverage, this is the honest state of
`testdata/vocals/` as a whole (15 files: 6 owner takes + 9 third-party refs):

- **Pitch range.** The whole set now spans roughly **65–1107 Hz at the frame
  extreme** (includes label octave noise per §6(b)/§11, so not to be read as
  clean coverage) and roughly **93–591 Hz at the percentile level** from the
  owner's takes, extended by the third-party set's **142–535 Hz** band — the
  third-party set is denser in the mid-to-upper range (six of eight singers
  are female, several in an upper tessitura) rather than pushing the
  boundaries outward. Framed the way the controller's brief put it: **roughly
  100–1100 Hz across eight singing voices plus one speaker**, reading that as
  the frame-level extremes rather than the tighter percentile band — the
  extremes of the engine's declared 65–1100 Hz range are still not densely
  exercised by clean, high-agreement material, only touched at the noisy
  edges.
- **Voices.** Nine performers total across the whole corpus (owner + 8
  third-party singers), one of whom (`ref06`/`ref07`, Seerat Ain Alam)
  appears twice. Still far short of measuring speaker generalisation at any
  scale, but no longer the single-voice set §6 warned about for the owner's
  takes alone.
- **Speech coverage — still a caveat, now a real number instead of an absent
  one.** `ref09_lecture_hindi.wav` is the **only spoken material in the
  entire reference corpus**, owner's and third-party's combined. Its
  measured voiced fraction (36.6%) and consensus rate (41.4%) — both roughly
  half of any sung take's figures — are the clearest evidence that AC6
  (voiced/unvoiced classification, <5% frame error) genuinely needs speech to
  be stressed properly, exactly as §6(c) predicted before any speech sample
  existed. **But it is projected lecture speech** — a trained/practised
  speaking register, clear diction, sustained vocal effort for a 15-minute
  recording, presumably close-mic'd or at least clean-recorded for a teaching
  video. **Any AC6 figure measured from this file alone will be optimistic
  relative to the product's actual target: a phone voice note, with its
  hesitations, closer background noise, and far more casual articulation.**
  This file closes the "no speech at all" gap in kind but not in difficulty.
- **No off-pitch singing.** Nothing in either the owner's takes or this
  third-party set sings *deliberately* off-pitch — every voice is trying to
  land the correct pitch, however well or poorly. **This whole reference
  corpus can demonstrate that a pitch DETECTOR works. It cannot demonstrate
  that a pitch CORRECTOR works**, because there is no recording anywhere in
  it of a singer intentionally flat or sharp for the corrector to pull back
  into tune. That gap is untouched by this task and stays open for whatever
  addresses AC-level correction testing.
- **`ref08` is a flagged hazard file, not a confirmed one** — see §12.2.
  Score it, but do not treat a clean detector number from it as proof the
  file is solo vocal.

---

## 14. Reproducing (third-party set)

```bash
.venv/bin/python tools/label_f0.py --stats-json /tmp/stats.json testdata/vocals/ref0*.wav
```

Same tool, same settings as §7. All nine files in one invocation: **4 m 46 s**
wall time, exit code 0, no memory issue (§12.3). The `.wav` inputs are not in
the repository — see §8 for provenance and the decode command used to
produce them from the owner's local source files.
