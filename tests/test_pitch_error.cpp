// T1.1 done-criteria (tasks.md): a harness that measures pitch-detection
// error in cents across the full 65-1100 Hz range, for ANY PitchDetector,
// feeding audio in fixed blocks as a host would, returning per-frequency
// error plus summary statistics, and able to name which frequencies failed
// a threshold rather than just reporting that some did.
//
// This project has repeatedly found tests that pass on the null
// hypothesis (see tasks.md T0.9's discovered-constraint note). A measuring
// instrument is worse than none if it can be silently wrong, so the first
// two TEST_CASEs below hand-compute an expected cents() value or an
// expected sweep result and assert the harness reproduces it exactly --
// not just "some number came out", but "this specific number came out".
// Only after that is the harness trusted to measure the real
// AutocorrelationDetector.
#include "doctest.h"
#include "opentune/AutocorrelationDetector.h"

#include "support/PitchError.h"
#include "support/Signals.h"

#include <cmath>
#include <cstddef>
#include <limits>
#include <memory>
#include <vector>

using opentune::AutocorrelationDetector;
using opentune::PitchDetector;
using opentune::PitchEstimate;
using opentune::test::cents;
using opentune::test::defaultSweepFrequenciesHz;
using opentune::test::measureOneFrequency;
using opentune::test::PitchErrorSample;
using opentune::test::runPitchSweep;
using opentune::test::SweepConfig;
using opentune::test::SweepResult;

namespace {

constexpr double kSampleRate = 48000.0;
constexpr int kBlockSize = 256;

// Always reports a fixed frequency, voiced, from the very first block --
// no ring buffer, no warm-up needed. Used to feed the harness a
// *controlled* answer so its arithmetic can be checked against a value
// computed by hand, independent of any real detection algorithm.
class FixedFrequencyDetector final : public PitchDetector {
public:
    explicit FixedFrequencyDetector(float reportedHz) : m_reportedHz(reportedHz) {}

    void prepare(double /*sampleRate*/, int /*maxBlockSize*/) override {}
    void reset() noexcept override {}

    PitchEstimate process(const float* /*block*/, int /*n*/) noexcept override {
        return PitchEstimate{m_reportedHz, 1.0f, true};
    }

private:
    float m_reportedHz;
};

// Always reports unvoiced, regardless of input. Used to prove the harness
// treats "never locked on" as a reportable failure (NaN errorCents), not a
// silently-skipped pass.
class NeverVoicedDetector final : public PitchDetector {
public:
    void prepare(double /*sampleRate*/, int /*maxBlockSize*/) override {}
    void reset() noexcept override {}

    PitchEstimate process(const float* /*block*/, int /*n*/) noexcept override {
        return PitchEstimate{0.0f, 0.0f, false};
    }
};

} // namespace

TEST_CASE("cents() reproduces hand-computed values, including octaves and unison") {
    // Unison: identical frequencies are 0 cents apart, exactly.
    CHECK(cents(440.0f, 440.0f) == doctest::Approx(0.0f));

    // Octave up: 1200 * log2(880/440) = 1200 * log2(2) = 1200 * 1 = 1200,
    // exactly, by hand.
    CHECK(cents(880.0f, 440.0f) == doctest::Approx(1200.0f));

    // Octave down: 1200 * log2(220/440) = 1200 * log2(0.5) = 1200 * -1 =
    // -1200, exactly, by hand.
    CHECK(cents(220.0f, 440.0f) == doctest::Approx(-1200.0f));

    // Two octaves up: 1200 * log2(1760/440) = 1200 * log2(4) = 2400.
    CHECK(cents(1760.0f, 440.0f) == doctest::Approx(2400.0f));

    // One semitone up, constructed as exactly 440 * 2^(1/12) so the
    // expected answer is exactly 100 cents by the definition of a
    // semitone (1200 cents / 12 semitones per octave = 100 cents each).
    const float oneSemitoneUp = 440.0f * static_cast<float>(std::pow(2.0, 1.0 / 12.0));
    CHECK(cents(oneSemitoneUp, 440.0f) == doctest::Approx(100.0f));
}

TEST_CASE("cents() is NaN-safe for non-positive or non-finite input") {
    CHECK(std::isnan(cents(0.0f, 440.0f)));
    CHECK(std::isnan(cents(440.0f, 0.0f)));
    CHECK(std::isnan(cents(-440.0f, 440.0f)));
    CHECK(std::isnan(cents(440.0f, -440.0f)));
    CHECK(std::isnan(cents(std::numeric_limits<float>::quiet_NaN(), 440.0f)));
    CHECK(std::isnan(cents(440.0f, std::numeric_limits<float>::quiet_NaN())));
    CHECK(std::isnan(cents(std::numeric_limits<float>::infinity(), 440.0f)));
}

TEST_CASE("measureOneFrequency reproduces a hand-computed error for a controlled detector") {
    // FixedFrequencyDetector always reports 445 Hz, voiced, from block one
    // -- no warm-up needed, so every post-warm-up frame contributes the
    // identical value cents(445, 440), and the median of identical values
    // is that value exactly.
    FixedFrequencyDetector detector(445.0f);
    detector.prepare(kSampleRate, kBlockSize);

    SweepConfig config;
    config.sampleRate = kSampleRate;
    config.blockSize = kBlockSize;
    config.toneLengthSamples = 9600;
    config.warmupBlocks = 4;

    const PitchErrorSample sample = measureOneFrequency(detector, 440.0f, config);

    const double expected = 1200.0 * std::log2(445.0 / 440.0);
    CAPTURE(expected);
    CHECK(sample.frequencyHz == doctest::Approx(440.0f));
    CHECK(static_cast<double>(sample.errorCents) == doctest::Approx(expected));
    CHECK(sample.voicedFraction == doctest::Approx(1.0));
    CHECK(sample.totalFrames > 0);
    CHECK(sample.voicedFrames == sample.totalFrames);
}

TEST_CASE("measureOneFrequency reports NaN error, not zero, for a detector that never locks on") {
    NeverVoicedDetector detector;
    detector.prepare(kSampleRate, kBlockSize);

    SweepConfig config;
    config.toneLengthSamples = 4096;

    const PitchErrorSample sample = measureOneFrequency(detector, 440.0f, config);

    CHECK(std::isnan(sample.errorCents));
    CHECK(sample.voicedFraction == doctest::Approx(0.0));
    CHECK(sample.voicedFrames == 0);
}

TEST_CASE("runPitchSweep aggregates known per-frequency errors into a hand-checkable median, "
          "mean, and max") {
    // Three frequencies, each paired with a detector reporting a
    // *specific known* error so the aggregation logic (not just a single
    // measurement) can be checked by hand:
    //   440 Hz -> reports 440 Hz exactly       => error   0 cents
    //   220 Hz -> reports 220 * 2^(100/1200)    => error 100 cents
    //   880 Hz -> reports 1760 Hz (one octave)  => error 1200 cents
    // Sorted: [0, 100, 1200] -> median 100, mean (0+100+1200)/3 = 433.33,
    // max 1200. All three are distinct, so a bug that mixed up which
    // statistic is which would be caught.
    const std::vector<float> frequencies = {440.0f, 220.0f, 880.0f};
    const float reportedFor220 = 220.0f * static_cast<float>(std::pow(2.0, 100.0 / 1200.0));
    const std::vector<float> reportedHz = {440.0f, reportedFor220, 1760.0f};

    std::size_t callIndex = 0;
    const auto makeDetector = [&]() -> std::unique_ptr<PitchDetector> {
        REQUIRE(callIndex < reportedHz.size());
        auto detector = std::make_unique<FixedFrequencyDetector>(reportedHz[callIndex]);
        ++callIndex;
        return detector;
    };

    SweepConfig config;
    config.toneLengthSamples = 4096;
    config.warmupBlocks = 2;

    const SweepResult result = runPitchSweep(makeDetector, frequencies, config);

    REQUIRE(result.perFrequency.size() == 3);
    CHECK(static_cast<double>(result.perFrequency[0].errorCents) == doctest::Approx(0.0));
    CHECK(static_cast<double>(result.perFrequency[1].errorCents) == doctest::Approx(100.0));
    CHECK(static_cast<double>(result.perFrequency[2].errorCents) == doctest::Approx(1200.0));

    CHECK(static_cast<double>(result.medianAbsErrorCents) == doctest::Approx(100.0));
    CHECK(static_cast<double>(result.meanAbsErrorCents) == doctest::Approx(1300.0 / 3.0));
    CHECK(static_cast<double>(result.maxAbsErrorCents) == doctest::Approx(1200.0));
    CHECK(result.voicedFraction == doctest::Approx(1.0));

    // failuresAbove names WHICH frequencies failed, not just that some
    // did: at a 50-cent threshold, 220 Hz (100 cents) and 880 Hz
    // (1200 cents) fail; 440 Hz (0 cents) does not.
    const std::vector<PitchErrorSample> failures = result.failuresAbove(50.0f);
    REQUIRE(failures.size() == 2);
    CHECK(failures[0].frequencyHz == doctest::Approx(220.0f));
    CHECK(failures[1].frequencyHz == doctest::Approx(880.0f));

    // At a looser 1500-cent threshold nothing fails.
    CHECK(result.failuresAbove(1500.0f).empty());
}

TEST_CASE("failuresAbove treats a never-voiced frequency as a failure at any threshold") {
    const std::vector<float> frequencies = {440.0f};
    const auto makeDetector = [] { return std::make_unique<NeverVoicedDetector>(); };

    SweepConfig config;
    config.toneLengthSamples = 4096;

    const SweepResult result = runPitchSweep(makeDetector, frequencies, config);

    REQUIRE(result.perFrequency.size() == 1);
    CHECK(std::isnan(result.perFrequency[0].errorCents));
    CHECK(result.voicedFraction == doctest::Approx(0.0));

    // Even an enormous threshold cannot let a missing answer pass -- a
    // detector that reports nothing is not a detector with zero error.
    const std::vector<PitchErrorSample> failures = result.failuresAbove(1.0e6f);
    REQUIRE(failures.size() == 1);
    CHECK(failures[0].frequencyHz == doctest::Approx(440.0f));

    // With no non-NaN error anywhere, there is nothing to average: the
    // summary statistics stay NaN rather than silently reporting 0.
    CHECK(std::isnan(result.medianAbsErrorCents));
    CHECK(std::isnan(result.meanAbsErrorCents));
    CHECK(std::isnan(result.maxAbsErrorCents));
}

TEST_CASE("defaultSweepFrequenciesHz spans the declared 65-1100 Hz range at semitone spacing") {
    const std::vector<float> frequencies = defaultSweepFrequenciesHz();

    REQUIRE(!frequencies.empty());
    // C2 = MIDI 36 = 440 * 2^((36-69)/12) ~= 65.406 Hz, hand-computable
    // from the MIDI->Hz formula in engine/CLAUDE.md.
    CHECK(static_cast<double>(frequencies.front()) == doctest::Approx(65.406).epsilon(0.01));
    // C6 = MIDI 84 = 440 * 2^((84-69)/12) = 440 * 2^1.25 ~= 1046.502 Hz.
    CHECK(static_cast<double>(frequencies.back()) == doctest::Approx(1046.502).epsilon(0.01));

    for (float hz : frequencies) {
        CHECK(hz >= 65.0f);
        CHECK(hz <= 1100.0f);
    }
    // Strictly increasing (one entry per ascending semitone).
    for (std::size_t i = 1; i < frequencies.size(); ++i) {
        CHECK(frequencies[i] > frequencies[i - 1]);
    }
}

// --- The Stage 1 baseline: measure the real AutocorrelationDetector ------
//
// This is the harness actually being used for its purpose (tasks.md T1.1):
// AutocorrelationDetector is Stage 0's deliberately naive detector, with a
// documented octave-too-high bias (docs/decisions/0003) on signals with a
// weak fundamental. A pure sine always has a strong fundamental, so this
// does not exercise that bias directly -- but it is exactly the
// measurement Stage 1's YinDetector must be compared against on equal
// terms, so this test only asserts the harness *runs to completion and
// produces a well-formed result* on the real detector, not a specific
// accuracy bound (asserting a bound here would either be so loose it
// proves nothing, or would pin down a number Stage 1 is expected to
// improve on -- and either way would not belong to a task whose brief is
// "build the harness", not "grade the detector"). The actual measured
// numbers are reported alongside this task, not baked into an assertion.
TEST_CASE("runPitchSweep runs the real AutocorrelationDetector across the declared range") {
    // A reduced, musically-spaced subset of the full range (not all 49
    // semitones): AutocorrelationDetector's O(lags * window) search is
    // deliberately expensive (~1M multiply-adds per block, per its own
    // header comment), so this keeps the suite's runtime in line with
    // test_autocorrelation_detector.cpp's existing six-frequency sweep at
    // the same tone length.
    const std::vector<float> frequencies = {65.41f, 130.81f, 220.0f, 293.66f,
                                            440.0f, 587.33f, 880.0f, 1046.50f};

    const auto makeDetector = [] { return std::make_unique<AutocorrelationDetector>(); };

    SweepConfig config;
    config.sampleRate = kSampleRate;
    config.blockSize = kBlockSize;
    config.toneLengthSamples = 9600;
    config.warmupBlocks = 16;

    const SweepResult result = runPitchSweep(makeDetector, frequencies, config);

    REQUIRE(result.perFrequency.size() == frequencies.size());
    CHECK(result.voicedFraction >= 0.0);
    CHECK(result.voicedFraction <= 1.0);
    for (const PitchErrorSample& sample : result.perFrequency) {
        CHECK(sample.voicedFraction >= 0.0);
        CHECK(sample.voicedFraction <= 1.0);
        CHECK(sample.totalFrames > 0);
    }
}
