#include "opentune/Engine.h"

#include <algorithm>
#include <cstddef>

namespace opentune {

namespace {

// --- FR15 clamp bound, derivation --------------------------------------
//
// What this clamp actually guards, today (see docs/decisions/0004 for the
// corrected history -- an earlier draft of this comment argued from a
// scenario that turned out not to be reachable; that reasoning was wrong,
// and is kept only as a cross-reference there, not repeated here):
//
//   - The Stage 0 ScaleQuantizer's nearest-semitone rounding mathematically
//     bounds every ratio *this* pipeline can currently produce to +/-50
//     cents (ratio ~0.9715-1.0293, verified both analytically and by a
//     numerical sweep -- see docs/decisions/0004 and docs/lessons.md).
//     This clamp's bound is well outside that range, so **it does not bind
//     today**: for Stage 0, it is inert insurance, not an active guard.
//   - Stage 5's gapped scales change that: HarmonicMinor and Pentatonic
//     have 3-semitone gaps between some allowed notes, so their worst-case
//     nearest-note distance is 1.5 semitones (ratio 2^(1.5/12) ~= 1.0905)
//     -- inside this clamp's +/-2-semitone bound, with margin, but no
//     longer negligible against it the way Stage 0's 50 cents is.
//   - Stage 4's `retuneMs` introduces a target that glides toward (rather
//     than snapping instantly to) the quantized pitch, and -- more
//     importantly for this clamp -- introduces the possibility of a target
//     computed from a *different* block's detected pitch than the one the
//     ratio is currently being applied to, once smoothing carries state
//     across blocks. That reintroduces exactly the "stale target vs. a
//     since-changed detected pitch" gap this clamp is built to catch,
//     which Stage 0's single-block, always-fresh computation does not
//     create.
//   - A misbehaving PitchDetector (a future implementation with a bug, or
//     one that does not respect the declared 65-1100 Hz range) is not
//     bounded by any of the above reasoning at all -- FR15 is written
//     as a property of the Engine, not of "whichever detector currently
//     exists," and this clamp is what makes that property actually hold
//     regardless of which PitchDetector is injected.
//
// The bound chosen here is +/-2 semitones (in equal temperament, one
// semitone is a frequency ratio of 2^(1/12) ~= 1.0595):
//     kMaxPitchRatio = 2^( 2/12) ~= 1.1225   (two semitones sharp)
//     kMinPitchRatio = 2^(-2/12) ~= 0.8909   (two semitones flat)
//
// Musical reasoning: a *pitch corrector*'s job is to nudge a performance
// that is already close to the intended note, not to reinterpret which
// note was sung. A singer landing within a semitone or two of their target
// is a normal (if quite imprecise) real performance worth correcting; a
// detected pitch a full octave (12 semitones, ratio 2.0) from the target
// is not a performance to correct at all -- it is a detection error (or a
// wildly wrong note), and a corrector cannot fix a wrong note by
// resampling, only mangle it further (ResampleCorrector.h documents
// exactly this: it starves above ratio 1.0 and exhausts its bounded
// history buffer below it, so neither direction is safe far from unity).
// Two semitones clears Stage 5's worst gapped-scale case (1.5 semitones,
// 1.0905) with real margin, while remaining an order of magnitude away
// from the octave-scale ratios that are actually dangerous for the
// corrector, and sits comfortably inside the "roughly 0.94-1.06" range the
// task brief itself cites as typical of real correction ratios.
//
// Private to this translation unit (an anonymous-namespace free function,
// not an Engine member): the exact numeric bound is an implementation
// detail, not part of Engine's public contract, so widening it later (e.g.
// once Stage 5 lands) is an internal change, not a public-API break. It is
// exercised indirectly through Engine::process() in
// tests/test_engine.cpp, using a PitchCorrector test double that records
// the ratio it was actually given.
constexpr float kMinPitchRatio = 0.890898718f;
constexpr float kMaxPitchRatio = 1.122462048f;

float clampPitchRatio(float rawRatio) noexcept {
    return std::clamp(rawRatio, kMinPitchRatio, kMaxPitchRatio);
}

} // namespace

Engine::Engine(PitchDetector& detector, ScaleQuantizer& quantizer, PitchCorrector& corrector,
               Params params)
    : m_detector(detector), m_quantizer(quantizer), m_corrector(corrector), m_params(params) {
    // `params` is copied, not moved: Params is a small, trivially-copyable
    // struct (see the static_assert in tests/test_params.cpp) with nothing
    // for a move to meaningfully steal, so std::move here would just
    // dress up a copy as a move that cannot happen.
}

void Engine::prepare(double sampleRate, int maxBlockSize) {
    m_detector.prepare(sampleRate, maxBlockSize);
    m_corrector.prepare(sampleRate, maxBlockSize);

    // Params is the single source of truth for which scale is active
    // (specs.md section 7.1's Params.h doc); push it into the quantizer
    // here so a caller cannot accidentally leave the injected
    // ScaleQuantizer configured with a different scale than the Params it
    // also handed the Engine. See the prepare() doc comment in Engine.h
    // for the two consequences this implies (shared-quantizer clobbering,
    // and no live scale change yet).
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
    // through *uncorrected* -- but it still goes THROUGH the corrector, at
    // pitchRatio 1.0, rather than being copied straight to the output.
    //
    // This used to be a direct copy, on the reasoning that a copy gives
    // bit-identical passthrough by construction. That reasoning is sound
    // about one block in isolation and wrong about a stream, because it
    // creates two paths with two different latencies. A corrector with
    // latency (SignalsmithCorrector's STFT costs 140 ms) emits corrected
    // audio late, while a direct copy emits bypassed audio immediately --
    // so at every voiced/unvoiced boundary the bypassed audio arrives
    // ahead of the corrected audio it should follow, overwriting its tail.
    //
    // Measured before the fix, on a tone with a 300 ms silent gap: the gap
    // came out 260 ms long and 40 ms early, having swallowed 140 ms of
    // corrected tail. In real singing every consonant and every breath is
    // such a boundary, so this was not an edge case (tasks.md D6).
    //
    // Routing everything through one path makes latency uniform whatever
    // corrector is installed. The cost is that passthrough is no longer
    // bit-identical for a lossy corrector -- an STFT round trip is not
    // transparent. FR2 asks for *uncorrected*, not *bit-identical*, and
    // unity ratio applies no pitch correction, so the requirement is met.
    // A latency-consistent stream matters far more to the ear than exact
    // sample values in the gaps between phrases.
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
        m_corrector.process(in, out, n, 1.0f);
        return;
    }

    // Quantize: nearest pitch the current scale allows (FR3).
    const float targetHz = m_quantizer.snap(estimate.frequencyHz);

    // Compute ratio: PitchCorrector.h defines pitchRatio as output
    // frequency / input frequency. The corrector's input is (believed to
    // be) at estimate.frequencyHz; the desired output is targetHz.
    const float rawRatio = targetHz / estimate.frequencyHz;

    // Clamp (FR15) before the corrector ever sees this ratio. See the
    // anonymous namespace above for what this bound is and what it
    // currently does (and does not) guard against.
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

} // namespace opentune
