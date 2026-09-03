#pragma once

#include "opentune/Params.h"
#include "opentune/PitchCorrector.h"
#include "opentune/PitchDetector.h"
#include "opentune/ScaleQuantizer.h"

namespace opentune {

// The whole pipeline, wired together: detect -> quantize -> compute ratio ->
// clamp -> correct. See specs.md section 7 for the block diagram and
// section 7.1 for this class's exact contract.
//
// Engine owns none of its three collaborators' *implementations* -- it is
// handed a `PitchDetector`, a `ScaleQuantizer`, and a `PitchCorrector` at
// construction (constitution V: implementations are chosen at construction,
// never by preprocessor branching) and drives them purely through their
// interfaces. This is what lets a host swap `AutocorrelationDetector` for
// `YinDetector`, or `ResampleCorrector` for `SignalsmithCorrector`, later
// (Stage 1, Stage 2) without recompiling this class at all.
//
// Engine *does* own a `Params` by value: the user-facing control surface
// (specs.md section 7.1). Stage 0 only reads `params.scale` (handed to the
// quantizer); `strength`, `retuneMs`, `humanizeCents`, and
// `preserveFormants` are stored but not yet acted on -- see FR7-FR10, which
// land in T4.2 onward.
class Engine {
public:
    // Wires the engine to its three collaborators and its initial
    // parameters. The three references are stored as references (not
    // copied, not owned): the caller constructs and owns the concrete
    // PitchDetector/ScaleQuantizer/PitchCorrector instances -- typically
    // for their entire lifetime, e.g. a CLI's `main()` -- and this Engine
    // merely drives them each block. `params` is copied by value (it is a
    // small, trivially-copyable struct, see Params.h) so the Engine has its
    // own mutable copy independent of whatever the caller does with theirs
    // afterward.
    //
    // Not real-time safe and not noexcept: this runs once, off the audio
    // thread, before streaming starts (constructing an Engine is a setup
    // operation, exactly like each collaborator's own constructor).
    Engine(PitchDetector& detector, ScaleQuantizer& quantizer, PitchCorrector& corrector,
           Params params);

    // One-time (or parameter-change-time) setup: forwards to the detector's
    // and corrector's own `prepare()` (specs.md section 7.1 -- both
    // interfaces declare `prepare(sampleRate, maxBlockSize)`). ScaleQuantizer
    // has no `prepare()`: it is a pure, stateless-beyond-its-Scale function
    // (see ScaleQuantizer.h), so there is nothing for it to size or
    // allocate. May allocate (constitution II) -- never called from the
    // audio thread while streaming.
    //
    // Also pushes `m_params.scale` into the injected `ScaleQuantizer` (see
    // Engine.cpp), making Params the single source of truth for which
    // scale is active. Two undocumented consequences worth knowing before
    // wiring up a host: (1) the injected ScaleQuantizer is not owned by
    // this Engine -- if two Engines are ever constructed sharing the *same*
    // ScaleQuantizer instance, each one's prepare() clobbers the other's
    // scale; give each Engine its own ScaleQuantizer. (2) there is
    // currently no way to change the scale *after* prepare() without
    // calling prepare() again (which also re-prepares the detector and
    // corrector, discarding their buffered state) -- a lock-free
    // "change scale live" path is FR10/FR5 territory, not yet built.
    void prepare(double sampleRate, int maxBlockSize);

    // Resets the detector's and corrector's internal state back to their
    // post-prepare() state (their own `reset()`), without freeing or
    // reallocating anything. Real-time safe (noexcept), per constitution
    // II -- called on the audio thread, e.g. when transport restarts.
    void reset() noexcept;

    // Runs one block through the full pipeline:
    //   1. detect  -- ask the PitchDetector for this block's PitchEstimate.
    //   2. If unvoiced, bypass detection-driven correction entirely (FR2):
    //      copy `in` to `out` unmodified and stop. This is deliberately a
    //      direct copy, not a call into the corrector with pitchRatio 1.0
    //      -- FR2 requires unvoiced audio to pass through *untouched*, and
    //      a direct copy is bit-identical passthrough by construction,
    //      independent of whatever a given PitchCorrector implementation's
    //      own unity-ratio behaviour happens to be.
    //   3. quantize -- snap the detected frequency to the nearest pitch the
    //      current `params.scale` allows (ScaleQuantizer::snap()).
    //   4. compute ratio -- targetHz / detectedHz: how much to multiply
    //      pitch by to move from what was sung to what should have been
    //      sung.
    //   5. clamp -- see the FR15 clamp comment in the .cpp for the exact
    //      bound and what it actually guards against today (an internal
    //      implementation detail, not part of this class's public
    //      contract -- see docs/decisions/0004 for why a ratio anywhere
    //      near a full octave cannot currently arise from this pipeline at
    //      all, and what the clamp is a backstop against instead).
    //   6. correct -- hand the clamped ratio to the PitchCorrector, which
    //      writes the shifted signal to `out`.
    //
    // Always produces exactly `n` output samples (block contract,
    // engine/CLAUDE.md), for any `n <= maxBlockSize`. Real-time safe: never
    // allocates, throws, locks, logs, or blocks (noexcept). It never reads
    // `in[i + 1]` while producing `out[i]` beyond what the detector's and
    // corrector's own internal buffering already holds (no lookahead,
    // constitution III). `in` and `out` must not overlap (same requirement
    // as PitchCorrector::process()).
    void process(const float* in, float* out, int n) noexcept;

    // How many samples of delay this engine's pipeline introduces, so a
    // host can compensate (e.g. keep a dry/wet mix in sync). Stage 0's
    // pipeline introduces no delay of its own beyond whatever the injected
    // PitchCorrector reports -- the detector's own analysis lag is *not*
    // output latency (constitution III: "Analysis lag is not output
    // latency," since the detector only ever looks at samples already
    // received, never future ones) -- so this simply forwards the
    // corrector's `latencySamples()`. Real-time safe (noexcept): it reports
    // a value fixed by `prepare()`, not one that changes per block.
    int latencySamples() const noexcept;

private:
    // FR15's clamp (the bound, and the function that applies it) is an
    // implementation detail of process() and deliberately not part of this
    // class's public surface -- see the anonymous namespace in Engine.cpp.
    // Making the exact numeric bound part of the public API would turn
    // widening it (e.g. for Stage 5's sparser scales) into a public-API
    // change instead of an internal one. It is exercised indirectly, via
    // process(), in tests/test_engine.cpp using a corrector test double
    // that records the ratio it was actually given.

    PitchDetector& m_detector;
    ScaleQuantizer& m_quantizer;
    PitchCorrector& m_corrector;
    Params m_params;
};

} // namespace opentune
