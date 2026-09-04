# 0005 — Vendor Signalsmith Stretch and pull the real corrector into Stage 0
**Date:** 2026-09-04 · **Resolves:** Q3 (partially), D5 · **Status:** accepted

## Context
`ResampleCorrector` (T0.7) was deliberately the wrong algorithm — the plan was that its
ugliness would make Stage 2's problem audible. Probing the assembled pipeline showed it is
not merely ugly, it is **unusable for more than a few seconds**: it cannot sustain any pitch
ratio ≠ 1.0. Above 1.0 it starves; below 1.0 it exhausts its bounded history. Measured
collapse to a full-scale block-rate staircase: flat 435 Hz at 0.68 s, 415 Hz at 3.38 s, sharp
445 Hz at 4.84 s. Only exactly-in-tune input survives (see `tasks.md` D5).

That breaks the Stage 0 checkpoint, which is the entire point of the stage: the owner records
a vocal, listens, and writes the artifact list that becomes the Stage 1–2 agenda. Listening to
a buzzing staircase produces no such list.

## Options
- **Accept and document.** Rejected: the checkpoint would demonstrate a few seconds of audio,
  and the stage's deliverable — a human listening log — would be worthless.
- **Wrap the read position** so it loops instead of pinning. Rejected: fixes only the
  starvation half and leaves a 3–5 second time bomb in the drift half. Half a fix, and it
  buys a naive algorithm we would discard anyway.
- **Pull Stage 2's corrector forward.** Chosen. The interface (T0.6) was designed for exactly
  this swap, so the cost is vendoring plus one implementation — no change to `Engine`, the
  CLI, or any caller.

## Decision
Vendor **Signalsmith Stretch** and implement `SignalsmithCorrector` behind the existing
`PitchCorrector` interface, in Stage 0. Owner approved 2026-09-04.

## Licence verification (constitution I)
Verified from the projects' own licence text, not from memory or a search summary:

| Library | Licence | Note |
|---|---|---|
| `signalsmith-stretch` | **MIT** — "Copyright (c) 2022 Geraint Luff / Signalsmith Audio Ltd." | Header-only |
| `signalsmith-linear` | **MIT** — "Copyright (c) 2025 Signalsmith Audio" | Pulled in by `stretch.h` via `signalsmith-linear/stft.h` |

The chain terminates: `linear` has no submodules. `stretch`'s only submodule is `cmd/util`,
which serves its command-line demo and is **not** vendored. Both licences permit the closed-
source and App Store futures Q1 leaves open.

## Consequences
- **The Stage 0 checkpoint becomes meaningful.** The owner will hear real pitch correction,
  and the listening log will describe artifacts of a serious algorithm rather than of a bug.
- **`ResampleCorrector` is kept, not deleted.** It stays as the naive baseline the interface
  was built to contrast against, and T2.3/T2.6 still A/B against it. Its D5 limitation is now
  documented and pinned by a test rather than being a trap.
- **A real-time risk moves earlier and must not be forgotten.** Signalsmith is STFT-based, so
  it has inherent latency, and its default preset is tuned for quality rather than for a
  20 ms budget (AC4). Stage 0 is offline, so this costs nothing now — but T2.5 (latency
  accounting) and T3.8 (verify ≤ 20 ms) become load-bearing, and the preset choice is a real
  decision, not a default to accept.
- **Allocation discipline needs checking, not assuming.** The header uses `std::vector`,
  `std::function` and `std::random`. Constitution II forbids allocation on the audio path, so
  `SignalsmithCorrector` must confine all of it to `prepare()` and the implementation must
  demonstrate that, not assert it.
- Stage 2 keeps its quality work: verification against the naive baseline, formant
  preservation, latency accounting, and the A/B listening pass.
