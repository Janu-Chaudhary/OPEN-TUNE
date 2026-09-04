# 0011 — Confirm YIN's chosen lag against its even multiples (step 3b)
**Date:** 2026-09-04 · **Resolves:** T1.8 / D12's octave-high failure · **Status:** accepted

## Context
D12 established that YIN's remaining octave errors on real material are genuine — the labels are
right roughly 9:1 where spectral evidence is decisive — and that they are octave **HIGH**, not low,
concentrated at 260–320 Hz. The mechanism is *diplophonia*: the odd harmonics are present but
14–20 dB below the envelope, around 1–4% of the energy. That makes `d'(P/2) = 0.0444` clear the
0.1 threshold before `d'(P) = 0.0106` is ever reached, so step 3's first-crossing walk stops at the
half period. D10's relative threshold is inactive there (`2·d'min` ≈ 0.016).

Eight candidate fixes were measured in D12 and all eight rejected: each traded an octave-high error
for an octave-low one, regressed the synthetic set, and still left take04 near 7%.

**The real blocker was the test set, not the algorithm.** The synthetic cases contained H1-only
attenuation and zero-odd-harmonics with nothing between them, and take04's condition is precisely
that middle ground. Unfixed, the engine scored **0.000% on the existing 32 cases and 71.4% on the
new graded family** — the set could be perfect and blind to the failure carrying 82% of real
octave errors.

## The insight
A **true** period cannot be beaten by its own double: at 2P the waveform still matches, so `d'`
there is comparable, not smaller. A **half** period is different — it is beaten at every *even*
multiple, because the odd-harmonic mismatch that makes P/2 imperfect is antiperiodic in the lag
and cancels whenever the lag is an even multiple of P/2.

That asymmetry is checkable after the fact, without touching the threshold that D10 already tuned.

## Decision
Add **step 3b**, a post-hoc confirmation. Promote a chosen lag L to 2L only when both hold:

    d'(2L) < 0.50 · d'(L)      and      d'(4L) < 0.70 · d'(L)

**The 4L condition is what the eight rejected candidates lacked.** Without it the rule breaks the
existing synthetic set at a 0.25 ratio; with it the set holds 0.000% out to 1.0. Both constants sit
mid-plateau over ~17 000 measured frames. A third condition at 3L was measured and dropped as
inert.

Step 3b runs *after* the voicing gate, so voicing is untouched — verified bit-identical across
90 471 blocks, which means AC6 and recall cannot move.

## Consequences — measured, and signed
| | before | after |
|---|---|---|
| AC7, existing 32 synthetic cases | 0.000% | **0.000%** |
| New graded family, 6–24 dB (18 cases) | 66.7% high | **0.142%** |
| New graded family, 30 dB (3 cases) | 100% | **100% — not repaired** |
| AC1 / AC2 | 0.21 ¢ / 96.10% | **0.21 ¢ / 96.100%** |
| Real takes, pooled | 3.28% (1.96 hi / 1.32 lo) | **2.43% (0.82 hi / 1.61 lo)** |
| take04 | 10.90% | **7.53%** |

- **AC7 still fails on real recordings: 2.43% against a <1% bar.** This is not a pass and must not
  be reported as one.
- **The trade is favourable but not free.** Octave-high fell 1.96 → 0.82 while octave-low rose
  1.32 → 1.61, and independent spectral adjudication says those extra low errors are **genuine**:
  of the 339 remaining YIN-low frames, the evidence favours the label on 72.3%. take02 is one frame
  worse (1.73 → 1.75%) and is recorded as a regression rather than rounded away.
- **At 30 dB odd-harmonic attenuation the signal is genuinely closer to periodic at P/2**, and the
  rule correctly declines to promote. That is arguably the right answer rather than a failure —
  but it is unproven by ear, and at 14–20 dB down this is the region where listeners may hear the
  upper octave with roughness.
- **Verification needed a third instrument.** The generator's existing two verification methods
  both fail on this family *in the same way the detector fails* — one counts mean-crossings, which
  double; the other takes the shortest strong correlation peak, which is P/2. A heterodyne
  phase-slope method was added and calibrated against a positive and two negative controls first.
  The first negative control was itself wrong and flagged itself, which is exactly the check
  `docs/assumption-log.md` §1 asks for.

## Open
- A listening checkpoint on take04, 45–50 s. Nobody has heard the passage this whole decision is
  about.
- Whether AC7's <1% bar is right for real recordings at all, given that no label source available
  to this project is octave-reliable and 27.7% of the remaining disputed frames favour the detector.
