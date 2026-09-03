# 0002 — dr_wav for WAV file I/O
**Date:** 2026-09-04 · **Resolves:** part of T0.10 · **Status:** accepted

## Context
The Stage 0 CLI must read a vocal recording and write a corrected one. WAV is the only
format needed. Constitution I requires a permissive licence and owner approval before any
dependency enters the repository.

## Options
- **libsndfile** — the obvious industry choice, handles every format. **Rejected outright:
  LGPL.** Constitution I forbids it without exception, and adopting it would permanently
  remove both the permissive and the proprietary licence options for OpenTune, and block
  iOS distribution.
- **Hand-written WAV parser** — no dependency at all. Rejected on value: correctly handling
  8/16/24/32-bit integer, float, and multi-channel input is a real amount of fiddly code
  that teaches nothing about pitch correction and would add days to Stage 0.
- **dr_wav** — public domain / MIT-0, single header. Won.

## Decision
dr_wav, vendored to `third_party/dr_wav/`. Owner approved on 2026-09-04, scoped to dr_wav
alone; any further dependency needs its own approval.

## Consequences
- Host-only. `dr_wav.h` is never included from `engine/`, so constitution IV (engine purity)
  is untouched — the engine still depends on nothing but the standard library.
- Same author as **miniaudio**, which specs.md §11 already selects for Stage 3 real-time
  audio I/O. Consistent conventions across both, and one less unfamiliar API later.
- `DR_WAV_IMPLEMENTATION` must appear in exactly one translation unit. It sits in the test
  file during T0.10 and moves to the CLI's own source in T0.11.
- Public domain means no attribution obligation, but we vendor the licence text anyway so
  the dependency set stays auditable at a glance.
