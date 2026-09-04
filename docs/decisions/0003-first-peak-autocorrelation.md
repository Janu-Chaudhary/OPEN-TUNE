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
  octave-**high**.

  **Corrected 2026-09-04 (T1.2), and the original claim here was too broad.** This record
  asserted that a weak or missing fundamental — telephone band, belted notes, male vowels where
  H2 dominates — would make the first qualifying peak the first harmonic. Measured, it does not.
  Normalised autocorrelation at half the true period:

  | Signal | r(P) | r(P/2) |
  |---|---|---|
  | Control, H1–H6 | 1.0000 | 0.0002 |
  | Fundamental removed entirely | 1.0000 | 0.2004 |
  | H2-dominant, H1 at −20 dB | 1.0000 | 0.5868 |
  | **Even harmonics only** | 1.0000 | **1.0000** |

  Removing H1 does not create an octave ambiguity, because the surviving **odd** harmonics are
  not periodic at P/2 and destructively interfere there. The condition is stricter than this
  record implied: the signal must be *genuinely* periodic at the shorter lag, which needs every
  odd harmonic gone. And in that case reporting 2·f0 is not really an error — the waveform does
  have that period; only human perception insists on the missing fundamental.

  So the bias is real but far narrower than stated. T1.2 pins both behaviours with tests.
- The 0.6 normalised-correlation threshold does double duty: peak qualification and final
  voicing. A harmonic peak on a real voice clears 0.6 comfortably, which is the precise
  mechanism of the octave-too-high error.
- **Stage 1 must start from this fact, not from the textbook assumption.** T1.4's cumulative
  mean normalised difference is chosen to fix a too-low bias; this detector leans too high.
  AC7 (<1% octave errors) is measured against real voices, where this bias is the live risk.
- Reviewed and endorsed as correctly reasoned and in scope by an independent review on
  2026-09-04, which also caught that the header initially documented the opposite bias.
