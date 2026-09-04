# 0006 — AC4 is reachable with Signalsmith; the real constraint is low-pitch resolution
**Date:** 2026-09-04 · **Resolves:** the AC4 feasibility question raised by 0005 · **Status:** accepted

## Context
Decision 0005 pulled Signalsmith Stretch into Stage 0 and flagged a risk: the corrector
reported **140 ms** of latency, against AC4's **20 ms** cap for the whole system. If that
140 ms were inherent to the algorithm, Stage 3's real-time monitor would need a different
approach entirely — which would be the largest re-plan the project has faced.

It is not inherent. The T0.13/T0.14 review measured it.

## What was measured
Latency equals `blockSamples` (with `splitComputation=false`), because `analysisLatency()`
and `synthesisLatency()` are each `blockSamples/2`. `SignalsmithStretch::configure()` is
therefore a continuous latency knob:

| block | interval | latency | ms @ 48 kHz |
|---|---|---|---|
| 4800 | 1200 | 4800 | 100.0 |
| 1920 | 480 | 1920 | 40.0 |
| **960** | **240** | **960** | **20.0** |
| 768 | 192 | 768 | 16.0 |
| 512 | 128 | 512 | 10.7 |
| 256 | 64 | 256 | 5.3 |

Pitch error, pure sines, one semitone up, 6 s runs, measured over the last 2 seconds:

| block (latency) | 440 Hz | 220 Hz | 110 Hz | 65 Hz |
|---|---|---|---|---|
| 4800 (100 ms) | +6.7 ¢ | +17.8 ¢ | +36.1 ¢ | +15.8 ¢ |
| 1920 (40 ms) | −0.7 ¢ | +25.1 ¢ | +14.1 ¢ | −9.3 ¢ |
| 960 (20 ms) | +28.8 ¢ | +14.1 ¢ | −15.6 ¢ | −21.9 ¢ |
| 768 (16 ms) | +21.5 ¢ | +10.4 ¢ | −0.7 ¢ | +15.8 ¢ |
| 512 (10.7 ms) | +17.8 ¢ | −11.8 ¢ | −23.1 ¢ | +28.2 ¢ |
| 256 (5.3 ms) | −11.8 ¢ | −26.9 ¢ | +43.3 ¢ | **+193.2 ¢** |

## Decision
**Stage 3 keeps Signalsmith.** AC4 is reachable with this library; no second pitch-shifting
algorithm is needed for real time. The block size becomes a Stage 3 tuning decision, not an
architectural one.

## Consequences
- **The real limit is spectral resolution at the bottom of the range, not latency.** A phase
  vocoder's analysis window must span enough periods of the fundamental to resolve it. 65 Hz
  (the `specs.md` §6 lower bound) has a 15.4 ms period, so a 20 ms window sees only ~1.3
  periods. At 256 samples (5.3 ms) the low end collapses completely — +193 cents, more than a
  whole tone wrong. Everything from 768 upward holds roughly ±30 cents across 65–440 Hz.
- **20 ms for the corrector alone leaves zero AC4 headroom.** AC4 caps the *round trip*:
  detector, host buffering and device I/O all have to fit too. Either AC4 is restated as a
  corrector-only budget, or the practical target is 10–16 ms and the bottom of the pitch range
  is where the cost lands. **This is a decision for T2.5, and it should be taken deliberately
  rather than discovered at T3.8.**
- **These numbers measure pitch tracking, not artifact quality.** Smearing, transient blur and
  "phasiness" all worsen as the window shrinks, and no measurement here captures that. Only a
  listening pass settles whether 16–20 ms *sounds* acceptable — which is precisely what the
  Stage 0 checkpoint and T2.6 exist for.
- **The asymmetric-window escape hatch is out of reach without patching vendored code.**
  `signalsmith-linear/stft.h` exposes `setInterval(interval, shape, asymmetry)`, which buys a
  longer analysis window at lower latency — but `SignalsmithStretch::configure()` never
  forwards it and the `stft` member is private. Using it means an upstream patch or a fork,
  which collides with constitution I's "vendored unmodified" rule. Recorded here so T2.5 knows
  the option exists and what it costs.
