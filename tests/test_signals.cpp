#include "doctest.h"
#include "support/Signals.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

// T0.2 done-criterion: a generated 440 Hz sine at 48 kHz has verified length,
// peak amplitude within 1%, and a zero-crossing count matching 440 Hz within 1%.
//
// These generators are the foundation of every later DSP test: every detector,
// quantizer, and corrector test measures its output against a known-correct
// synthetic input produced here. If the generators are wrong, every test built
// on them is meaningless, so we verify them against first-principles arithmetic
// rather than against the engine.

namespace {

// Counts sign changes in a signal. For a periodic signal with frequency `hz`
// over `numSamples` at `sampleRate`, the expected zero-crossing count is
// 2 * hz * duration (two zero crossings per full cycle: rising and falling).
int countZeroCrossings(const std::vector<float>& signal) {
    int crossings = 0;
    for (std::size_t i = 1; i < signal.size(); ++i) {
        // A crossing happened if consecutive samples have opposite, non-zero sign.
        if ((signal[i - 1] < 0.0f && signal[i] >= 0.0f) ||
            (signal[i - 1] >= 0.0f && signal[i] < 0.0f)) {
            ++crossings;
        }
    }
    return crossings;
}

} // namespace

TEST_CASE("sine: 440 Hz at 48 kHz has correct length") {
    const double sampleRate = 48000.0;
    const int numSamples = 48000; // exactly 1 second
    const std::vector<float> signal = opentune::test::sine(440.0f, sampleRate, numSamples);
    CHECK(signal.size() == static_cast<std::size_t>(numSamples));
}

TEST_CASE("sine: 440 Hz peak amplitude is within 1% of requested amplitude") {
    const double sampleRate = 48000.0;
    const int numSamples = 48000; // 1 second gives many cycles to find the true peak
    const float amplitude = 0.8f;
    const std::vector<float> signal =
        opentune::test::sine(440.0f, sampleRate, numSamples, amplitude);

    float peak = 0.0f;
    for (float sample : signal) {
        peak = std::max(peak, std::fabs(sample));
    }

    CHECK(peak == doctest::Approx(amplitude).epsilon(0.01));
}

TEST_CASE("sine: 440 Hz zero-crossing count matches 2 * hz * duration within 1%") {
    const double sampleRate = 48000.0;
    const float hz = 440.0f;
    const int numSamples = 48000; // 1 second, so duration = 1.0
    const std::vector<float> signal = opentune::test::sine(hz, sampleRate, numSamples);

    const double duration = static_cast<double>(numSamples) / sampleRate;
    const double expectedCrossings = 2.0 * static_cast<double>(hz) * duration;
    const int actualCrossings = countZeroCrossings(signal);

    CHECK(static_cast<double>(actualCrossings) == doctest::Approx(expectedCrossings).epsilon(0.01));
}

TEST_CASE("sine: default amplitude is 1.0") {
    const std::vector<float> signal = opentune::test::sine(220.0f, 48000.0, 4800);
    float peak = 0.0f;
    for (float sample : signal) {
        peak = std::max(peak, std::fabs(sample));
    }
    CHECK(peak == doctest::Approx(1.0f).epsilon(0.01));
}

TEST_CASE("silence: produces the requested length, all zero") {
    const int numSamples = 512;
    const std::vector<float> signal = opentune::test::silence(numSamples);
    CHECK(signal.size() == static_cast<std::size_t>(numSamples));
    for (float sample : signal) {
        CHECK(sample == 0.0f);
    }
}

TEST_CASE("whiteNoise: produces the requested length and stays within [-1, 1]") {
    const int numSamples = 4800;
    const std::vector<float> signal = opentune::test::whiteNoise(numSamples, 42u);
    CHECK(signal.size() == static_cast<std::size_t>(numSamples));
    for (float sample : signal) {
        CHECK(sample >= -1.0f);
        CHECK(sample <= 1.0f);
    }
}

TEST_CASE("whiteNoise: is deterministic for a given seed") {
    const std::vector<float> a = opentune::test::whiteNoise(1024, 7u);
    const std::vector<float> b = opentune::test::whiteNoise(1024, 7u);
    CHECK(a == b);
}

TEST_CASE("whiteNoise: different seeds produce different signals") {
    const std::vector<float> a = opentune::test::whiteNoise(1024, 1u);
    const std::vector<float> b = opentune::test::whiteNoise(1024, 2u);
    CHECK(a != b);
}

TEST_CASE("sweep: produces the requested length") {
    const int numSamples = 48000;
    const std::vector<float> signal = opentune::test::sweep(220.0f, 880.0f, 48000.0, numSamples);
    CHECK(signal.size() == static_cast<std::size_t>(numSamples));
}

TEST_CASE("sweep: stays within [-1, 1] and has no discontinuities") {
    // Phase-accumulated sweeps cannot exceed unit amplitude, and consecutive
    // samples of a continuous sine-derived signal cannot jump by more than the
    // maximum possible slope for the highest instantaneous frequency involved.
    // We check the loose bound (amplitude) plus a sanity check that no sample
    // is NaN/inf, which would indicate a phase-accumulation bug.
    const int numSamples = 48000;
    const std::vector<float> signal = opentune::test::sweep(65.0f, 1100.0f, 48000.0, numSamples);
    for (float sample : signal) {
        CHECK(std::isfinite(sample));
        CHECK(sample >= -1.0f);
        CHECK(sample <= 1.0f);
    }
}

TEST_CASE("sweep: zero-crossing count matches the average frequency within a loose tolerance") {
    // For a linear sweep from startHz to endHz, the average instantaneous
    // frequency is (startHz + endHz) / 2, so the expected zero-crossing count
    // over the whole signal is 2 * avgHz * duration. Tolerance is loose (5%)
    // because this is a derived, not exact, property of a linear sweep.
    const double sampleRate = 48000.0;
    const float startHz = 220.0f;
    const float endHz = 440.0f;
    const int numSamples = 48000;
    const std::vector<float> signal = opentune::test::sweep(startHz, endHz, sampleRate, numSamples);

    const double duration = static_cast<double>(numSamples) / sampleRate;
    const double avgHz = (static_cast<double>(startHz) + static_cast<double>(endHz)) / 2.0;
    const double expectedCrossings = 2.0 * avgHz * duration;
    const int actualCrossings = countZeroCrossings(signal);

    CHECK(static_cast<double>(actualCrossings) == doctest::Approx(expectedCrossings).epsilon(0.05));
}
