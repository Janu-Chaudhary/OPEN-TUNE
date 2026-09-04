# 0007 — What YIN's cumulative mean normalised difference actually does
**Date:** 2026-09-04 · **Resolves:** a false claim in `tasks.md` T1.4 · **Status:** accepted

## Context
`tasks.md` described T1.4 — YIN's cumulative mean normalised difference (CMND) — as "the step
that kills octave errors", and the brief handed to the implementer repeated it. The reasoning
sounded right: the running mean of the difference function grows with lag, so a candidate at
twice the true period 2P is divided by a larger number than one at P, and is therefore penalised.

The T1.3–T1.7 implementer asserted that as a test. **It failed.**

For a 300 Hz sine the running mean at lag 160 and at lag 320 are *equal* — 1490.69 both. The
mean plateaus after roughly one period, because beyond that the difference function is just
oscillating around a stable average. So P and 2P are divided by the same number and **tie
exactly**. CMND does not break the octave tie at all.

## What CMND actually buys
Two things, both real, neither the advertised one:

1. **It kills the too-HIGH family.** `d'(τ) >= 1` holds across the entire rising slope of the
   difference function — verified across all 342 lags below a 70 Hz half-period. A detector
   cannot latch onto a sub-period candidate, which is precisely the failure mode
   `AutocorrelationDetector`'s first-peak selection suffers from (see 0003).
2. **It puts every candidate on a common scale**, which is what makes step 3's *fixed*
   absolute threshold possible at all. Without normalisation the threshold would have to
   adapt to signal level.

**The octave tie is broken by T1.5**, the absolute threshold with best-candidate selection:
it takes the *first* candidate below the threshold, and P comes before 2P.

## Decision
`tasks.md` T1.4's description is corrected, and the header of `YinDetector` states what each
step actually contributes. A test now asserts the P/2P tie explicitly rather than hiding it.

## Consequences
- Anyone tuning octave behaviour later should look at the **threshold** (T1.5), not the
  normalisation. Chasing it in CMND would be chasing the wrong step, which is exactly the
  cost of leaving the original claim in place.
- Measured result, independently reproduced by the controller: on harmonics 1–8 with odd
  harmonics 14 dB down, `AutocorrelationDetector` reports exactly 2×f0 on **10 of 10**
  fundamentals; `YinDetector` gets **0 of 10** wrong.
- This is the fourth plan claim in this project corrected by someone who went and measured —
  see `docs/lessons.md` L4. The pattern is now well enough established that a brief asserting
  *why* an algorithm works should be treated as a hypothesis for the implementer to test, not
  as a specification to satisfy.
