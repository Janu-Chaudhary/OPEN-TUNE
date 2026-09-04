# 0010 — take04's AC7 excess is a real detector error, and no step-3 rule fixes it
**Date:** 2026-09-04 · **Resolves:** D12 · **Status:** accepted

## Context
After D10, YIN scores **AC7 0.000% on the synthetic set** and **3.28% across the owner's six
real takes**, against a <1% bar — and it is concentrated: take04 10.90%, take02 1.73%, take06
1.06%, take03 1.01%, take01 0.55%, take05 0.03%. take04 alone carries 82% of the total.

D12 named three things as unestablished. This record settles all three.

The first mattered most, and it is the one this project keeps getting wrong (`docs/lessons.md`
L5, four instances): **are take04's LABELS octave-correct?** They are a 2-of-3 consensus of
three window-based estimators, and period doubling is the failure all three share — HPS has a
documented octave-DOWN bias of its own. `testdata/vocals/MANIFEST.md` §6(b) already measures the
label track's own octave inconsistency at 3.43% on take04 and says outright that take02, take04
and take06 cannot resolve a sub-1% octave-error rate. So "the labels are right" was treated as a
hypothesis, not a premise.

## The adjudication, and how it was validated before use
`tools/d12_octave_evidence.py` judges each disputed frame on evidence independent of YIN *and*
of all three labelling estimators: the **magnitude spectrum** alone. If the higher candidate
`fHigh` is the fundamental, there is nothing at `fLow`, `3*fLow`, `5*fLow`. If `fLow` is the
fundamental, those partials exist and sit **on the spectral envelope** traced by their even
neighbours — a subharmonic from a period-doubled voice puts energy there too, but 10–20 dB
*below* that envelope. Two measurements, then: partial presence above the local floor, and
envelope deficit.

Both were **calibrated against a control before being used to judge anything** (L5, third
instance). The control is free and in-domain: the frames of the same take where YIN and the
labels agree. Scoring the agreed f0 as `fLow` must come out positive; scoring `agreed f0 / 2` as
`fLow` must come out negative. On take04, n=1500 each:

| | positive control | negative control | separation |
|---|---|---|---|
| partials above floor | median 29.9 dB | median 11.2 dB | 7.1% / 9.5% error at 18.9 dB |
| envelope deficit | median 1.3 dB | median 21.4 dB | 5.8% / 5.3% error at 10.1 dB |

Frames in the grey band between the two controls are reported **ambiguous**, not guessed.

## Finding 1 — the labels are right, by about 9 to 1
Of take04's 847 octave-disputed frames: **428 favour the LABEL, 48 favour YIN, 371 ambiguous.**
Across all six takes, of 1092 disputed frames: **641 label, 61 YIN, 390 ambiguous.**

So D12 is **not** a measurement artifact. Even crediting YIN with every ambiguous frame, the real
takes sit at **1.92%–3.09% octave errors** — failing the <1% bar at the most charitable reading.
take04 alone is **5.51%–10.28%**. There is no reading of this evidence in which the engine is fine.

Two limits on that claim, stated because they are real. This method can only speak to frames
where YIN and the labels *disagree*; where both are wrong the same way it is silent. And take04's
own controls are markedly worse (5–7% error each way) than take02/take06/take01's (0.5–1%), which
is itself a measurement: take04's material is spectrally the messiest in the set.

## Finding 2 — the direction was backwards, and so was the pitch region
The lead in D12 was octave-**LOW** errors rising with pitch. Measured, take04 is the opposite on
both counts:

| | share of take04's 10.90% |
|---|---|
| octave **HIGH** (YIN reads 2x the label) | **7.78 points** |
| octave LOW | 3.11 points |

| label f0 | frames | octave error | high | low |
|---|---|---|---|---|
| 200–260 Hz | 1155 | 1.39% | 0.26% | 1.13% |
| **260–320 Hz** | **5404** | **13.43%** | **10.25%** | 3.18% |
| 320–420 Hz | 729 | 6.58% | 6.31% | 0.27% |
| 420–560 Hz | 103 | 34.95% | 0.00% | 34.95% |

The excess is in the **middle** of take04's range, not the top. The octave-low-with-pitch effect
is real but tiny in absolute terms: 420–560 Hz holds 103 frames, 36 errors, 0.46 of the 10.90
points. take04 is also the *only* take with a large octave-high population — take02's 1.73% is
100% octave-low.

## Finding 3 — the mechanism, and it is not D10's
The errors concentrate in one passage (roughly 42–49 s, peaking 45–50 s) where the label holds a
sustained ~312 Hz and YIN reads ~624 Hz. Measured on the spectrum at t = 48.263 s, relative to
the local noise floor:

| partial | 312 | 468 | 624 | 780 | 936 | 1248 | 1560 |
|---|---|---|---|---|---|---|---|
| dB above floor | 35.8 | 20.5 | **56.6** | 27.4 | 31.3 | 34.5 | 30.0 |

The odd multiples of 312 Hz **are present**, 30–36 dB above the floor — the label did not invent
them — but they sit 14–20 dB below the envelope the even ones trace. This is a **diplophonic /
weak-odd-harmonic** voice: the odd harmonics carry only ~1–4% of the energy, so the waveform
genuinely repeats to within a few percent at *half* the period.

A replica of steps 1–3 (validated: it reproduces the shipped detector's f0 to within 0.5% on
100% of 4491 take04 frames, and its voiced flag on 100%) gives the exact values, in the shape
decision 0009 uses for D10:

| t | d'(P/2) | d'(P) | d'(2P) | threshold | chosen |
|---|---|---|---|---|---|
| 47.863 s | **0.0753** | 0.0260 | 0.0131 | 0.1000 | P/2 |
| 48.263 s | **0.0444** | 0.0106 | 0.0079 | 0.1000 | P/2 |
| 48.567 s | **0.0544** | 0.0207 | 0.0116 | 0.1000 | P/2 |

`d'(P/2)` sits **below** the paper's fixed 0.1, so step 3's first-crossing walk stops at the half
period and never reaches P — where `d'` is 3–5x lower. Note the threshold column: `2 * dPrimeMin`
is ~0.016 here, far under 0.1, so **D10's relative threshold is inactive on these frames**. This
is a pure paper-YIN failure. Confirmed independently from the pre-D10 dump: 581 octave-high
frames before D10, 605 after — D10 neither caused it nor materially worsened it. D10 removed 473
octave-low frames on take04 (715 → 242) and touched the high ones by 24.

## Finding 4 — the synthetic set cannot see this, by construction
`tools/synth_voices.py` builds `weak_fundamental` (H1 attenuated 20 dB) and
`missing_fundamental` (H1 removed). Neither creates an octave ambiguity, and T1.2 already
measured why (`docs/lessons.md` L4, second instance): H3, H5 survive at full strength and do not
cancel at P/2. The only ambiguous synthetic case is `even_harmonics_only`, where the odd
harmonics are **zero** and P/2 is genuinely the period — a behaviour pin, excluded from scoring.

take04's condition is the middle of that range and it is **absent from the set**: *every* odd
harmonic attenuated together, by 14–20 dB. That is precisely why AC7 reads 0.000% on synthetic
and 10.90% on take04. D12's second unestablished question, answered: **no, the synthetic set does
not cover take04's worst passages.**

## Options measured — every one of them regresses the synthetic pass
All measured with the validated replica, over the 32 scored synthetic cases and take04:

| step-3 rule | synth octave | synth ≤15¢ | take04 octave (high / low) |
|---|---|---|---|
| `max(0.1, 2·dmin)` — shipped | **0.000%** | **96.07%** | 10.83% (7.80 / 3.03) |
| `2·dmin` (drop the floor) | 5.963% | 77.19% | 7.80% (1.23 / 6.57) |
| `max(0.05, 2·dmin)` | 0.199% | 95.81% | 6.88% (2.00 / 4.88) |
| `max(0.03, 3·dmin)` | 0.182% | 95.85% | 6.94% (3.83 / 3.11) |
| + octave-up promotion `d'(2L) < 0.5·d'(L)` | 0.442% | 95.63% | 7.39% (2.57 / 4.83) |
| + promotion `d'(2L) < 0.35·d'(L)` | 0.191% | 95.88% | 7.70% (4.06 / 3.64) |

The pattern is one-for-one: **every rule that removes an octave-high error adds an octave-low
one.** It has to. Step 3's walk is short-to-long against a bar, so the bar's only degrees of
freedom are "earlier" and "later", and the two failure modes sit on opposite sides of it. The
best variant buys 3.9 points on take04 — leaving it at 7%, still 7x the bar — and costs the
synthetic 0.000%.

The octave-up promotion was worth measuring because the discriminating statistic really does
separate: on take04, `d'(2L)/d'(L)` has median 1.38 on agreed frames and 0.365 on octave-high
frames, with only 2.4% of agreed frames below 0.5. But 2.4% of the correct frames is larger than
the population it repairs, and the flipped frames become octave-*low*.

A crude causal cross-frame octave-continuity sketch (8-frame median period, promote/demote to the
nearest octave whose `d'` is within 1.5x) was also tried: take04 got **worse**, 10.83% → 11.95%,
because YIN flickers between 312 and 624 within the same sustained note, so the history is itself
contaminated. Decision 0009 rejected cross-frame continuity on the grounds that it buys "a wrong
lock persisting across frames". That prediction is now measured, not argued.

## Decision
1. **Ship no engine change under D12.** Every candidate fix regresses AC7 on the synthetic set
   from 0.000% and none brings take04 within 7x of the bar. Tuning step 3 until take04's number
   improved would be trading a measured pass for an unmeasured one.
2. **Record that AC7 fails on real material and why**, at 1.92%–3.09% — a floor and a ceiling,
   not a point estimate, because 390 of 1092 disputed frames are genuinely ambiguous.
3. **The synthetic set needs a case with all odd harmonics attenuated together** (sweep the
   attenuation 6/12/18/24 dB at ~300 Hz). Until it has one, a synthetic AC7 pass says nothing
   about this failure mode. This is the single highest-value follow-up here.
4. **The real fix is not in step 3.** It needs information step 3 does not have — either YIN's own
   step 6 "best local estimate" (a bounded search over recent frames, which is engine state and a
   design decision, not a constant tweak), or a Viterbi/HMM octave track over the `d'` curve. The
   crude continuity sketch above says a naive version of this makes things worse; it does not say
   a proper one would.

## Consequences
- **AC7 stands as PASSED on synthetic and FAILED on real material**, and the two are not in
  conflict — they measure different signal conditions, and §4 above says exactly which one the
  synthetic set is missing.
- take04's excess is **one voice condition in one passage**, not a general weakness: it is 82% of
  the total, concentrated at 260–320 Hz, in a diplophonic ~7-second stretch.
- The owner should know that on that passage the *perceptual* answer is not obvious either. Odd
  partials 14–20 dB down is the region where listeners hear the upper octave with roughness
  rather than the lower pitch cleanly. This record does **not** claim YIN sounds wrong there —
  nobody has listened. It claims YIN disagrees with the fundamental that is measurably present.
- `tools/d12_octave_evidence.py` stays, because the question recurs: it is the only instrument in
  this project that can adjudicate an octave dispute without trusting either party.
