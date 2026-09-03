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
    //   5. clamp -- see the kMinPitchRatio/kMaxPitchRatio comment in the
    //      .cpp for the exact bound and why (FR15). This is the step that
    //      keeps a detector's occasional octave error (ratio near 2.0 or
    //      0.5) from ever reaching the corrector.
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

    // FR15 -- clamps a raw pitch ratio (targetHz / detectedHz) to the
    // musical range this engine treats as a legitimate *correction*. See
    // the .cpp for the exact bound and its musical justification.
    //
    // Public and static -- unlike the rest of Engine's behaviour, which is
    // only observable by driving the whole detect->quantize->correct
    // pipeline through process() -- because this one piece of FR15's logic
    // is a pure function of a single float with no Engine state involved,
    // and the task brief specifically requires testing the clamp
    // *directly*, "with a stub detector returning a deliberately
    // octave-wrong estimate," rather than only through whatever ratio the
    // current ScaleQuantizer happens to be able to produce. See Engine.cpp
    // and tests/test_engine.cpp for why that distinction matters here: the
    // Stage 0 ScaleQuantizer's nearest-semitone quantization is
    // mathematically bounded to +/-50 cents of its input for any
    // frequency, so `snap(f)/f` alone can never actually reach a ratio
    // anywhere near this clamp's bound today -- this function is FR15's
    // defensive backstop regardless (against a misbehaving PitchDetector, a
    // future sparser scale, or a numerical edge case), and this is how it
    // is exercised directly.
    //
    // Real-time safe (noexcept): called once per voiced block from
    // process(), on the audio thread.
    static float clampPitchRatio(float rawRatio) noexcept;

    // The musical range `clampPitchRatio` enforces. See Engine.cpp for the
    // derivation (+/-2 semitones) and reasoning.
    static constexpr float kMinPitchRatio = 0.890898718f;
    static constexpr float kMaxPitchRatio = 1.122462048f;

private:
    PitchDetector& m_detector;
    ScaleQuantizer& m_quantizer;
    PitchCorrector& m_corrector;
    Params m_params;
};

} // namespace opentune
