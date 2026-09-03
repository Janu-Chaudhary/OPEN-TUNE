#include "opentune/Engine.h"

#include <algorithm>
#include <cstddef>

namespace opentune {

namespace {

// --- FR15 clamp bound, derivation --------------------------------------
//
// This clamp exists to stop a detection error from ever reaching a
// PitchCorrector as a wild ratio (specs.md section 7.1, FR15): the task
// brief that motivated this file documents two independent facts that
// compose into a real failure if this clamp is missing --
//   1. ResampleCorrector (the naive Stage 0 corrector) starves above
//      ratio 1.0: producing n output samples at ratio r needs r*n input
//      samples, but a real-time block only ever hands it n, so past
//      ratio 1.0 it falls back to holding its last sample and the result
//      becomes block-size dependent (see ResampleCorrector.h).
//   2. AutocorrelationDetector's octave errors lean high, not low (see
//      docs/decisions/0003-first-peak-autocorrelation.md).
// A ratio anywhere near 2.0 (an octave) is exactly where (1) is weakest,
// so it must never be allowed to reach the corrector.
//
// The bound chosen here is +/-2 semitones (in equal temperament, one
// semitone is a frequency ratio of 2^(1/12) ~= 1.0595):
//     kMaxPitchRatio = 2^( 2/12) ~= 1.1225   (two semitones sharp)
//     kMinPitchRatio = 2^(-2/12) ~= 0.8909   (two semitones flat)
//
// Musical reasoning: a *pitch corrector*'s job is to nudge a performance
// that is already close to the intended note, not to reinterpret which
// note was sung. A singer landing within a semitone or two of their
// target is a normal (if quite imprecise) real performance worth
// correcting; a detected pitch a full octave (12 semitones, ratio 2.0)
// from the target is not a performance to correct at all -- it is a
// detection error (or a wildly wrong note), and a corrector cannot fix a
// wrong note by resampling, only mangle it further. Two semitones gives
// comfortable headroom over the largest correction the Stage 0
// ScaleQuantizer can ever actually request (see the class comment on
// Engine::clampPitchRatio in Engine.h: its nearest-semitone rounding
// mathematically bounds every real correction to +/-50 cents, a quarter
// of this clamp's width) while remaining an order of magnitude away from
// the octave-scale ratios that are actually dangerous for the corrector.
// It is also comfortably inside the "roughly 0.94-1.06" range the task
// brief itself cites as typical of real correction ratios.
//
// (The two constants live on Engine itself, in Engine.h, as
// kMinPitchRatio/kMaxPitchRatio -- computed offline as 2^(+/-2/12) rather
// than with a runtime std::pow call here, since they are compile-time
// constants and this is a one-time, well-known derivation, not something
// that needs to be recomputed or kept "live" against a semitone count.)

} // namespace

// Defined here (rather than only declared in the header) because a
// constexpr static data member used only by odr-unevaluated contexts
// (doctest's Approx comparisons in tests/test_engine.cpp take it by
// reference) needs an out-of-class definition pre-C++17 semantics; with
// C++17 inline implicit linkage this is technically optional, but is kept
// explicit for clarity and to avoid relying on that (constitution:
// C++17, but no need to lean on its more obscure corners).
constexpr float Engine::kMinPitchRatio;
constexpr float Engine::kMaxPitchRatio;

Engine::Engine(PitchDetector& detector, ScaleQuantizer& quantizer, PitchCorrector& corrector,
               Params params)
    : m_detector(detector), m_quantizer(quantizer), m_corrector(corrector),
      m_params(std::move(params)) {}

void Engine::prepare(double sampleRate, int maxBlockSize) {
    m_detector.prepare(sampleRate, maxBlockSize);
    m_corrector.prepare(sampleRate, maxBlockSize);

    // Params is the single source of truth for which scale is active
    // (specs.md section 7.1's Params.h doc); push it into the quantizer
    // here so a caller cannot accidentally leave the injected
    // ScaleQuantizer configured with a different scale than the Params it
    // also handed the Engine.
    m_quantizer.setScale(m_params.scale);
}

void Engine::reset() noexcept {
    m_detector.reset();
    m_corrector.reset();
}

void Engine::process(const float* in, float* out, int n) noexcept {
    if (n <= 0) {
        return;
    }

    const PitchEstimate estimate = m_detector.process(in, n);

    // FR2 / bypass: unvoiced audio (silence, breath, consonants) passes
    // through completely untouched. This is a direct copy, not a call
    // into the corrector with pitchRatio 1.0 -- FR2 requires bit-identical
    // passthrough, which a direct copy guarantees by construction,
    // independent of whatever a given PitchCorrector's own unity-ratio
    // behaviour happens to be (PitchCorrector.h documents unity as merely
    // the *ideal*, not a guarantee, for a real implementation).
    //
    // `estimate.frequencyHz <= 0.0f` is checked too, defensively, even
    // though a well-behaved PitchDetector never reports frequencyHz <= 0
    // while voiced is true (PitchEstimate's own contract, PitchDetector.h)
    // -- if some future or buggy detector ever did, dividing by it below
    // would produce NaN/Inf rather than a musically meaningless-but-finite
    // ratio, and the clamp below only bounds *finite* values (std::clamp
    // on a NaN is undefined-ish/order-dependent). Treating that case as
    // unvoiced is the same "detection is fallible, do not trust it blindly"
    // spirit as the FR15 clamp itself.
    if (!estimate.voiced || estimate.frequencyHz <= 0.0f) {
        for (int i = 0; i < n; ++i) {
            out[static_cast<std::size_t>(i)] = in[static_cast<std::size_t>(i)];
        }
        return;
    }

    // Quantize: nearest pitch the current scale allows (FR3).
    const float targetHz = m_quantizer.snap(estimate.frequencyHz);

    // Compute ratio: PitchCorrector.h defines pitchRatio as output
    // frequency / input frequency. The corrector's input is (believed to
    // be) at estimate.frequencyHz; the desired output is targetHz.
    const float rawRatio = targetHz / estimate.frequencyHz;

    // Clamp (FR15) before the corrector ever sees this ratio.
    const float ratio = clampPitchRatio(rawRatio);

    // Correct: shift `in` to `out` by the clamped ratio. `strength` is not
    // applied yet (T4.2) -- Stage 0 always applies the full computed
    // ratio.
    m_corrector.process(in, out, n, ratio);
}

int Engine::latencySamples() const noexcept {
    // The detector's analysis window is lag, not output latency
    // (constitution III: "Analysis lag is not output latency") -- it only
    // ever looks at samples already received. So the engine's total
    // output delay is exactly whatever the corrector reports.
    return m_corrector.latencySamples();
}

float Engine::clampPitchRatio(float rawRatio) noexcept {
    return std::clamp(rawRatio, kMinPitchRatio, kMaxPitchRatio);
}

} // namespace opentune
