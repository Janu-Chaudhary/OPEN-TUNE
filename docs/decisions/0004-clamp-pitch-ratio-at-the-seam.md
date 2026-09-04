# 0004 — Clamp the pitch ratio in the Engine; the seam defect is starvation, not a huge ratio
**Date:** 2026-09-04 · **Resolves:** constraint discovered during T0.4/T0.7, corrected during T0.9 · **Status:** accepted, with a superseded rationale recorded below

## Context
When T0.4 and T0.7 were reviewed, a cross-component risk was identified: the detector's
octave errors lean **high** (see 0003), and `ResampleCorrector` misbehaves for pitch ratios
above 1.0. The conclusion drawn was that a detector octave error would request ratio ≈ 2.0
and produce garbage, and FR15 was written on that basis.

**That reasoning was wrong, and T0.9's implementer caught it.** The ratio is not
`target / detected_true_pitch`; it is `snap(f) / f`, and nearest-note rounding is bounded to
half a semitone by construction. Verified numerically across 65–1100 Hz: the ratio range is
exactly **0.971532 – 1.029302**, i.e. ±50.00 cents, matching 2^(±0.5/12). A detector octave
error does not inflate the ratio — it silently corrects toward the *wrong note* at a
perfectly ordinary ratio. That is a musical error, not a numerical explosion.

The real seam defect is worse and was found by probing the assembled pipeline:

**`ResampleCorrector` cannot sustain *any* ratio above 1.0.** Producing *n* output samples
requires *ratio × n* input samples; the block contract delivers exactly *n*. The deficit
accumulates every block until the read position is pinned at "hold the last available
sample". Measured on the real pipeline at 48 kHz / 256-sample blocks:

| Input | Direction | Ratio | First constant-valued output block |
|---|---|---|---|
| 435 Hz | flat | 1.0115 | 0.68 s |
| 415 Hz | flat | 1.0243 | 3.38 s |
| 445 Hz | sharp | 0.9888 | 4.84 s |
| 440 Hz | in tune | 1.0000 | never — bit-identical passthrough |

A ratio of 1.0115 is a *twenty-cent* correction — the most ordinary case there is. And the
sharp direction fails too, just later: below 1.0 the read position falls behind until it runs
off the old end of the bounded 2048-sample history. **Only an exactly-in-tune input
survives.** Output after collapse is a single constant value per block — a sample-and-hold at
the block rate, peak ~0.5, i.e. **full scale and loud**, not silence. The tests passed
because the longest is 0.25 s.

## Options
- **Make the corrector defensive about its ratio.** Rejected: it applies the ratio it is
  given; second-guessing arguments hides bugs and every future corrector re-implements it.
- **Rely on a better detector.** Rejected — and now known to be beside the point: the ratio
  was never the problem.
- **Clamp in the Engine.** Chosen, but understood correctly: this is cheap defence-in-depth
  for future configurations, **not** a fix for the starvation.

## Decision
`Engine` clamps the computed pitch ratio before passing it to the corrector (**FR15**,
implemented in T0.9 at ±2 semitones). The clamp is insurance against a future pipeline that
*can* produce a large ratio — Stage 4's `retuneMs` smoothing carries a target across blocks,
and Stage 5's gapped scales (pentatonic, harmonic minor have 3-semitone gaps) push nearest-
note snapping to 1.5 semitones, ratio 1.0905. Today it never binds.

The starvation is tracked separately as a Stage 0 limitation — see `tasks.md` D5.

## Consequences
- **FR15 is real but currently inert.** Anyone reading it should know it guards a future
  case, not a present one. Claiming otherwise would make the codebase lie about itself.
- **Stage 0 cannot sustain upward correction.** Any listening test on a flat performance
  will decay to near-silence within about a second. This must be known *before* the Stage 0
  checkpoint, or the owner will be listening for chipmunk artifacts in a file that is simply
  dying.
- The clamp will mask detector octave errors from the ear once a working corrector exists, so
  Stage 1 must measure octave-error rate against a labelled set (T1.0, T1.2, AC7) rather than
  trust that the output sounds acceptable.

## The wider lesson
Two lessons, both in `docs/lessons.md`: seam defects are invisible to per-task review (L3),
and a cross-component risk asserted from reasoning alone can be wrong in its specifics while
still pointing at something real (L4).
