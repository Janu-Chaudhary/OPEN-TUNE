# 0003 — First-peak rather than global-max lag selection
**Date:** 2026-09-04 · **Resolves:** design deviation found during T0.4 · **Status:** accepted

## Context
Textbook naive autocorrelation picks the lag with the highest correlation across the search
range. Implementing T0.4 revealed this fails on the project's own test signals.

For a pure sine of period P, with a comparison window of fixed length, the correlation is
`r(L) ≈ (W·A²/2)·cos(2πL/P)` — a cosine in lag, with **equal-height** maxima at every
`L = kP`. A global maximum among tied peaks is therefore decided by floating-point noise,
and can land on `k = 2`: an octave too low. Observed in practice as 880 Hz detected as
~80 Hz and 110 Hz as ~1090 Hz.

## Options
- **Global maximum** (textbook). Rejected: ties on pure tones, and its bias is octave-too-low.
- **First qualifying interior local peak.** Chosen: among tied candidates it favours the
  shortest period, which is the correct one. Operates purely on integer lags, so it remains
  plain autocorrelation.
- **Add YIN's cumulative mean normalised difference now.** Rejected as scope creep — that is
  T1.4, and Stage 0's ±20 cents bar is deliberately loose so it is not needed here.

## Decision
Select the first local peak in the correlation whose normalised value clears
`kMinNormalizedCorrelation`, excluding the range endpoints (near `minLag` the correlation is
still descending from the excluded lag-0 maximum, so an endpoint would be a false peak).

## Consequences
- **The octave bias is inverted, not eliminated.** Global-max errs octave-low; first-peak errs
  octave-**high**. On a voice with a weak or missing fundamental — telephone band, belted high
  notes, many male vowels where H2 dominates H1 — the first qualifying peak is the first
  harmonic, and the detector reports an octave too high.
- The 0.6 normalised-correlation threshold does double duty: peak qualification and final
  voicing. A harmonic peak on a real voice clears 0.6 comfortably, which is the precise
  mechanism of the octave-too-high error.
- **Stage 1 must start from this fact, not from the textbook assumption.** T1.4's cumulative
  mean normalised difference is chosen to fix a too-low bias; this detector leans too high.
  AC7 (<1% octave errors) is measured against real voices, where this bias is the live risk.
- Reviewed and endorsed as correctly reasoned and in scope by an independent review on
  2026-09-04, which also caught that the header initially documented the opposite bias.
