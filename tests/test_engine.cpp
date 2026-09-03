// Tests for opentune::Engine (T0.9).
//
// Engine wires PitchDetector -> ScaleQuantizer -> PitchCorrector together
// (specs.md section 7, section 7.1): detect -> quantize -> compute ratio ->
// clamp -> correct. This file covers the four "Done when" criteria from
// .superpowers/sdd/tasks/task-T0.9-brief.md:
//   1. A 445 Hz sine in produces roughly 440 Hz out.
//   2. Silence in produces bit-identical silence out (FR2).
//   3. Block boundaries introduce no discontinuity.
//   4. A detected pitch an octave from the target yields a clamped ratio.
//
// A note on criterion 4 (see the class comment on Engine::clampPitchRatio
// in Engine.h, and the T0.9 report, for the full reasoning): with the
// Stage 0 ScaleQuantizer, `snap(f)/f` is mathematically bounded to within
// +/-50 cents (a half semitone, ratio ~0.971-1.029) for *any* positive f,
// because it works by rounding to the *nearest* integer MIDI note in log
// space -- rounding to nearest can never be more than half a step away.
// That means the natural detect -> quantize -> ratio pipeline cannot
// actually manufacture a ratio anywhere near 2.0 or 0.5 today, regardless
// of how "octave-wrong" the detected frequency is: quantizing a wrong
// frequency to its own nearest semitone is still self-consistent and
// nearly a no-op ratio-wise. FR15's clamp is still implemented as specified
// (it is cheap insurance against a misbehaving detector, a future sparser
// scale, or numerical edge cases -- see Engine.cpp), and is tested directly
// below via its exposed static helper, exactly as the brief instructs
// ("test the clamp directly ... do not rely on provoking a real detector
// error") -- just not by routing an extreme ratio through the present
// ScaleQuantizer, which cannot currently produce one.
#include "doctest.h"
#include "opentune/AutocorrelationDetector.h"
#include "opentune/Engine.h"
#include "opentune/Params.h"
#include "opentune/PitchDetector.h"
#include "opentune/ResampleCorrector.h"
#include "opentune/ScaleQuantizer.h"

#include "support/Signals.h"

#include <cmath>
#include <cstddef>
#include <utility>
#include <vector>

namespace {

constexpr double kSampleRate = 48000.0;

double centsError(double measuredHz, double referenceHz) {
    return 1200.0 * std::log2(measuredHz / referenceHz);
}

// Same coarse-but-simple zero-crossing frequency estimator used in
// tests/test_resample_corrector.cpp -- see that file for the rationale.
double zeroCrossingFrequency(const std::vector<float>& signal, double sampleRate) {
    int crossings = 0;
    for (std::size_t i = 1; i < signal.size(); ++i) {
        if (signal[i - 1] < 0.0f && signal[i] >= 0.0f) {
            ++crossings;
        }
    }
    const double durationSeconds = static_cast<double>(signal.size()) / sampleRate;
    return static_cast<double>(crossings) / durationSeconds;
}

// A detector that ignores its input entirely and always reports a fixed
// estimate, regardless of how many samples it has seen or how the caller
// chunks them into blocks. Used to isolate a test to Engine's own
// per-block wiring (quantize -> ratio -> correct) from AutocorrelationDetector's
// own block-size-independent-but-buffer-filling behaviour, which is
// already covered by tests/test_autocorrelation_detector.cpp.
class StubPitchDetector final : public opentune::PitchDetector {
public:
    explicit StubPitchDetector(opentune::PitchEstimate estimate) : m_estimate(estimate) {}

    void prepare(double /*sampleRate*/, int /*maxBlockSize*/) override {}
    void reset() noexcept override {}

    opentune::PitchEstimate process(const float* /*block*/, int /*n*/) noexcept override {
        return m_estimate;
    }

private:
    opentune::PitchEstimate m_estimate;
};

} // namespace

TEST_CASE("Engine: a 445 Hz sine in produces roughly 440 Hz out") {
    opentune::AutocorrelationDetector detector;
    opentune::ScaleQuantizer quantizer({0, opentune::ScaleType::Chromatic});
    opentune::ResampleCorrector corrector;

    opentune::Params params;
    params.scale = {0, opentune::ScaleType::Chromatic};

    opentune::Engine engine(detector, quantizer, corrector, params);

    constexpr int kMaxBlockSize = 256;
    engine.prepare(kSampleRate, kMaxBlockSize);
    engine.reset();

    // Long enough to run well past AutocorrelationDetector's warm-up window
    // (~2200 samples at 48 kHz -- see AutocorrelationDetector.cpp) and
    // leave a large steady-state tail to measure a frequency from.
    constexpr int kTotalSamples = 12000;
    const std::vector<float> in = opentune::test::sine(445.0f, kSampleRate, kTotalSamples);
    std::vector<float> out(static_cast<std::size_t>(kTotalSamples), 0.0f);

    int offset = 0;
    while (offset < kTotalSamples) {
        const int n = std::min(kMaxBlockSize, kTotalSamples - offset);
        engine.process(in.data() + offset, out.data() + offset, n);
        offset += n;
    }

    // Measure only the tail, well after the detector's ring buffer has
    // filled and correction has settled to a steady ratio.
    constexpr int kMeasureStart = 6000;
    const std::vector<float> measured(out.begin() + kMeasureStart, out.end());

    const double measuredHz = zeroCrossingFrequency(measured, kSampleRate);
    const double cents = centsError(measuredHz, 440.0);
    CAPTURE(measuredHz);
    CAPTURE(cents);
    // Generous tolerance: AutocorrelationDetector is itself only accurate
    // to +/-20 cents (integer-lag quantization, see its header), and the
    // naive linear-interpolation ResampleCorrector adds its own small
    // error on top.
    CHECK(std::abs(cents) <= 35.0);
}

TEST_CASE("Engine: silence in produces bit-identical silence out") {
    opentune::AutocorrelationDetector detector;
    opentune::ScaleQuantizer quantizer({0, opentune::ScaleType::Chromatic});
    opentune::ResampleCorrector corrector;
    opentune::Params params;

    opentune::Engine engine(detector, quantizer, corrector, params);

    constexpr int kMaxBlockSize = 256;
    engine.prepare(kSampleRate, kMaxBlockSize);
    engine.reset();

    constexpr int kBlockSamples = 256;
    const std::vector<float> in = opentune::test::silence(kBlockSamples);
    // Poisoned with a distinctive non-zero value so a broken bypass (e.g.
    // writing nothing) is caught, not masked by a lucky zero-initialised
    // buffer.
    std::vector<float> out(static_cast<std::size_t>(kBlockSamples), -7.5f);

    engine.process(in.data(), out.data(), kBlockSamples);

    // FR2: unvoiced audio passes through *unmodified* -- exact equality,
    // not merely "quiet", per the task brief.
    for (float sample : out) {
        CHECK(sample == 0.0f);
    }
}

TEST_CASE("Engine: same input via different block sizes gives the same output") {
    // Isolates the invariance check to Engine's own per-block wiring (and
    // the corrector's already-documented ratio<=1 chunk-invariance, see
    // ResampleCorrector.h) by using a stub detector that always reports
    // the same estimate regardless of chunking -- so any difference
    // between the two runs below can only come from a bug in Engine
    // itself, not from AutocorrelationDetector's own buffer-filling being
    // chunk-size sensitive (a separate, already-covered concern).
    constexpr opentune::PitchEstimate kFixedEstimate{445.0f, 1.0f, true};

    // Quantizer snaps 445 -> 440 (a semitone below), so the ratio Engine
    // computes is 440/445 ~= 0.9888, which is < 1.0 -- exactly the regime
    // ResampleCorrector documents as genuinely chunk-invariant (it never
    // needs a sample beyond what has already arrived), so this test
    // exercises Engine's wiring, not ResampleCorrector's known ratio > 1.0
    // chunking quirk.
    constexpr int kTotalSamples = 2000;
    const std::vector<float> in = opentune::test::sweep(80.0f, 1000.0f, kSampleRate, kTotalSamples);

    auto runInBlocksOf = [&](int blockSize) {
        StubPitchDetector detector(kFixedEstimate);
        opentune::ScaleQuantizer quantizer({0, opentune::ScaleType::Chromatic});
        opentune::ResampleCorrector corrector;
        opentune::Params params;

        opentune::Engine engine(detector, quantizer, corrector, params);
        engine.prepare(kSampleRate, 256);
        engine.reset();

        std::vector<float> out(static_cast<std::size_t>(kTotalSamples), 0.0f);
        int offset = 0;
        while (offset < kTotalSamples) {
            const int n = std::min(blockSize, kTotalSamples - offset);
            engine.process(in.data() + offset, out.data() + offset, n);
            offset += n;
        }
        return out;
    };

    const std::vector<float> outSmallBlocks = runInBlocksOf(37);
    const std::vector<float> outLargeBlocks = runInBlocksOf(256);

    REQUIRE(outSmallBlocks.size() == outLargeBlocks.size());
    for (std::size_t i = 0; i < outSmallBlocks.size(); ++i) {
        CAPTURE(i);
        CHECK(outSmallBlocks[i] == outLargeBlocks[i]);
    }
}

TEST_CASE("Engine: FR15 clamp bounds an extreme pitch ratio, as an octave "
          "detection error would imply, rather than passing it through") {
    // Directly exercises the clamp (FR15) with the exact values a
    // detection octave error implies (specs.md section 7.1: "an octave
    // error produces a ratio near 2.0 or 0.5") -- per the file header
    // comment above, the *current* ScaleQuantizer cannot actually route
    // such a ratio through the full detect->quantize->correct pipeline
    // (its nearest-semitone quantization mathematically bounds
    // target/detected to +/-50 cents for any input), so this tests the
    // clamp function FR15 requires directly, as the task brief instructs.
    CHECK(opentune::Engine::clampPitchRatio(2.0f) ==
          doctest::Approx(opentune::Engine::kMaxPitchRatio));
    CHECK(opentune::Engine::clampPitchRatio(0.5f) ==
          doctest::Approx(opentune::Engine::kMinPitchRatio));

    // An octave and a half low/high: still clamped, not merely nudged.
    CHECK(opentune::Engine::clampPitchRatio(3.0f) ==
          doctest::Approx(opentune::Engine::kMaxPitchRatio));
    CHECK(opentune::Engine::clampPitchRatio(0.25f) ==
          doctest::Approx(opentune::Engine::kMinPitchRatio));

    // A legitimate small correction (well inside the clamp's musical
    // range) must pass through unchanged -- the clamp must not distort
    // ordinary corrections.
    CHECK(opentune::Engine::clampPitchRatio(1.02f) == doctest::Approx(1.02f));
    CHECK(opentune::Engine::clampPitchRatio(1.0f) == doctest::Approx(1.0f));

    // Exactly at the bound: unchanged (inclusive clamp).
    CHECK(opentune::Engine::clampPitchRatio(opentune::Engine::kMaxPitchRatio) ==
          doctest::Approx(opentune::Engine::kMaxPitchRatio));
}

// constitution II / engine/CLAUDE.md: process(), reset(), and
// latencySamples() are real-time functions and must never allocate,
// throw, lock, log, or block -- noexcept is the compiler-enforced half of
// that contract.
static_assert(noexcept(std::declval<opentune::Engine&>().process(nullptr, nullptr, 0)),
              "Engine::process must be noexcept (audio thread, constitution II)");
static_assert(noexcept(std::declval<opentune::Engine&>().reset()),
              "Engine::reset must be noexcept (audio thread, constitution II)");
static_assert(noexcept(std::declval<const opentune::Engine&>().latencySamples()),
              "Engine::latencySamples must be noexcept (audio thread, constitution II)");
static_assert(noexcept(opentune::Engine::clampPitchRatio(1.0f)),
              "Engine::clampPitchRatio must be noexcept (called from process(), audio thread)");
