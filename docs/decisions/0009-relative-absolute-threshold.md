# 0009 — YIN's absolute threshold is made relative
**Date:** 2026-09-04 · **Resolves:** D10 · **Status:** accepted

## Context
`YinDetector` implements YIN (de Cheveigné & Kawahara 2002) faithfully, including step 3's
**fixed** absolute threshold of 0.1: walk lags short-to-long, take the first whose `d'` dips
below it. That rule is the paper's, and the reason it beats a global minimum is stated in the
code — a global minimum is "a coin flip between P, 2P and 3P", while "first" means "shortest
period", so period doubling loses by construction.

Measured on the synthetic set, that fixed threshold is where **every one of YIN's octave errors
came from**, and every one was octave *down*. Excluding two breathy cases the rate was 0.02%;
including them, 1.34% against AC7's <1% bar.

Two distinct failures share the cause, and the first was missed initially:

1. **At HNR 10 dB the error is in step 3 itself**, not the fallback. `d'(P)` ≈ 0.117 sits just
   *above* 0.1 while `d'(2P)` ≈ 0.088 sits just *below*, so the first-crossing walk fires for the
   first time **at the octave**. Every residual frame in an intermediate experiment carried
   `cleared = true` — the fallback was never reached.
2. **At HNR 5 dB nothing clears 0.1 at all**, so the fallback runs, and the fallback took the
   global minimum — the very rule step 3 exists to avoid. 100% of frames fall back at that level.

The fixed threshold is the shared root: on a breathy voice the whole `d'` curve lifts, and a
constant bar drawn at 0.1 no longer sits where the periodicity structure is.

## Options
- **Repair the fallback only.** Rejected on measurement, not taste: it plateaus at **9.19%** on
  `breathy_hnr10db` at any tolerance, because those frames never reach the fallback.
- **Cross-frame octave continuity.** Rejected: buys a new failure mode — a wrong lock persisting
  across frames — and needs state, for a problem solvable without either.
- **Abstain: report `voiced=false` rather than guess.** Rejected with its cost measured: it would
  leave **28.2% of the owner's voiced sung frames uncorrected** (16.6% of all audio). AC7 counts
  only voiced frames, so this "passes" the criterion by declining to answer — an improvement in
  the number and a regression in the product.
- **Make the threshold relative.** Chosen.

## Decision
`threshold = max(kAbsoluteThreshold, kRelativeThreshold × min d')`, with
`kRelativeThreshold = 2.0`.

The bar now sits relative to the best periodicity the frame actually contains, so it tracks the
curve upward on a noisy voice instead of being crossed first by the octave. It can only ever
*raise* the bar above the paper's 0.1, so clean signals are **bit-identical**. And it subsumes the
fallback rather than relocating it: the global minimum always qualifies under its own doubled
value, so the "coin flip" branch is deleted, not moved.

`kRelativeThreshold = 2.0` sits inside a measured plateau — 2.0 through 4.0 give identical results
across all 33 cases. 1.75 leaves 0.11% residual. At 6 and above the mirror hazard appears, and it
shows up as *falling recall* rather than octave-high errors, because step 5 rejects short-lag noise
candidates instead of publishing them.

## Consequences
- **AC7 0.000%, AC2 96.10% on synthetic — both pass.** The two breathy cases went from 29.04% and
  27.27% octave errors to 0.00%, and both now sit at 100% within ±15 cents. AC1 unchanged at
  0.21 cents.
- **Not bought by reclassification.** On frames voiced in *both* runs, octave errors went 130 → 0
  and within-15 went 10884 → 11127. Voiced fractions are bit-identical on 30 of 33 cases; 43
  frames (0.37%) do turn unvoiced, all in the two breathy cases, because they now report the true
  period whose `d'` genuinely exceeds the 0.2 voicing gate instead of an octave whose `d'` did not.
- **This is a documented deviation from the paper.** Anyone comparing against published YIN results
  should know the threshold is no longer the paper's fixed 0.1.
- **`kVoicedAperiodicityMax = 0.2` now does more work than when T1.7 chose it.** The voicing gate
  is what those 43 frames now fall foul of, and it was set provisionally. It deserves re-derivation
  against the synthetic set rather than inheriting a provisional value.
- **Real takes improved but do not pass**: 8.08% → 3.28%, with take04 alone at 10.90% against
  ≤1.73% for every other take. Tracked as D12, and the first thing to check there is whether
  take04's own labels are octave-correct — they come from window-based estimators that share the
  ambiguity they are being used to judge.
