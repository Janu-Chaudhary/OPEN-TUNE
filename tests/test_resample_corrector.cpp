#include "doctest.h"
#include "opentune/ResampleCorrector.h"

#include "support/Signals.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

// T0.7 done-criteria (tasks.md): a 440 Hz sine at ratio 2.0 produces an
// 880 Hz sine (+/-20 cents); ratio 1.0 is bit-identical to input.
// engine/CLAUDE.md's "three tests minimum" also requires a silence-shaped
// test and a block-boundary test; the task brief additionally requires a
// no-NaN/inf test. See .superpowers/sdd/tasks/task-T0.7-brief.md.
//
// IMPORTANT (see ResampleCorrector.h for the full explanation): because this
// corrector consumes input from `in` faster than real time when
// pitchRatio > 1, its read position races ahead of what has actually
// arrived and it must fall back to holding once it runs out -- this happens
// partway through the very first call already, and gets worse on every
// later call. The ratio-2.0 test below therefore measures only a prefix of
// one large, single-call block, comfortably inside the region the read
// position can still reach with genuinely fresh samples. The block-boundary
// test uses a ratio <= 1.0, where this "runs out of input" situation never
// arises (see the header), so chunking cannot affect the result.

namespace {

constexpr double kSampleRate = 48000.0;

double centsError(double measuredHz, double referenceHz) {
    return 1200.0 * std::log2(measuredHz / referenceHz);
}

// Estimates frequency from zero crossings: counts upward (negative-to-
// non-negative) crossings across `signal` and divides by the elapsed time.
// This is a coarse but simple and easy-to-verify-by-hand measure, exactly
// as tests/support/Signals.h's `sine()` doc comment describes.
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

} // namespace

TEST_CASE("ResampleCorrector: 440 Hz sine at ratio 2.0 reads out around 880 Hz "
          "(+/-20 cents)") {
    // One large block so the whole test stays inside a single process() call:
    // this corrector's read position only ever gets to use samples already
    // handed to it in the *same* call (see header) without holding, so a
    // single big call gives the longest possible run of genuinely
    // interpolated (non-held) output to measure a frequency from.
    constexpr int kBlockSamples = 20000;

    opentune::ResampleCorrector corrector;
    corrector.prepare(kSampleRate, kBlockSamples);
    corrector.reset();

    const std::vector<float> in = opentune::test::sine(440.0f, kSampleRate, kBlockSamples);
    std::vector<float> out(static_cast<std::size_t>(kBlockSamples), 0.0f);

    corrector.process(in.data(), out.data(), kBlockSamples, 2.0f);

    // The read position advances at 2x, so it exhausts the block's 20000
    // fresh samples after roughly 20000/2 = 10000 output samples. Measure
    // well inside that: the first 8000 output samples are guaranteed to
    // come from genuine interpolation, not the post-exhaustion hold, and
    // give the zero-crossing count enough periods (~700 at 880 Hz) that
    // rounding a partial final cycle at the window's edge cannot skew the
    // measured frequency by much.
    constexpr int kMeasureSamples = 8000;
    const std::vector<float> measured(out.begin(), out.begin() + kMeasureSamples);

    const double measuredHz = zeroCrossingFrequency(measured, kSampleRate);
    const double cents = centsError(measuredHz, 880.0);
    CAPTURE(measuredHz);
    CAPTURE(cents);
    CHECK(std::abs(cents) <= 20.0);
}

TEST_CASE("ResampleCorrector: pitchRatio 1.0 is bit-identical to the input") {
    constexpr int kBlockSamples = 512;

    opentune::ResampleCorrector corrector;
    corrector.prepare(kSampleRate, kBlockSamples);
    corrector.reset();

    // A signal with no repeating structure (a sweep, not a sine) makes an
    // accidental "looks right" match from a bug (e.g. reading a stale or
    // wrapped-around sample) far less likely than an all-equal-period
    // signal would.
    const std::vector<float> in = opentune::test::sweep(80.0f, 1000.0f, kSampleRate, kBlockSamples);
    std::vector<float> out(static_cast<std::size_t>(kBlockSamples), -1.0f);

    corrector.process(in.data(), out.data(), kBlockSamples, 1.0f);

    // Exact equality, not doctest::Approx: this is the task's strongest
    // assertion (task brief) and proves the read-position arithmetic has
    // no drift at unity ratio.
    REQUIRE(out.size() == in.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
        CAPTURE(i);
        CHECK(out[i] == in[i]);
    }

    // Also true across a second call, continuing the same stream: the
    // read position must still track the input exactly, not just on the
    // very first call.
    const std::vector<float> in2 =
        opentune::test::sweep(1000.0f, 80.0f, kSampleRate, kBlockSamples);
    std::vector<float> out2(static_cast<std::size_t>(kBlockSamples), -1.0f);
    corrector.process(in2.data(), out2.data(), kBlockSamples, 1.0f);
    for (std::size_t i = 0; i < in2.size(); ++i) {
        CAPTURE(i);
        CHECK(out2[i] == in2[i]);
    }
}

TEST_CASE("ResampleCorrector: silence in, silence out, at several ratios") {
    constexpr int kBlockSamples = 256;

    for (float ratio : {0.5f, 1.0f, 2.0f}) {
        CAPTURE(ratio);
        opentune::ResampleCorrector corrector;
        corrector.prepare(kSampleRate, kBlockSamples);
        corrector.reset();

        const std::vector<float> in = opentune::test::silence(kBlockSamples);
        std::vector<float> out(static_cast<std::size_t>(kBlockSamples), 12.3f);

        corrector.process(in.data(), out.data(), kBlockSamples, ratio);

        for (float sample : out) {
            CHECK(sample == 0.0f);
        }
    }
}

TEST_CASE("ResampleCorrector: no NaN or infinity in the output, at several ratios "
          "and across several blocks (including past the point where input runs out)") {
    constexpr int kBlockSamples = 256;
    constexpr int kNumBlocks = 8;

    for (float ratio : {0.25f, 0.5f, 1.0f, 1.5f, 2.0f, 3.0f}) {
        CAPTURE(ratio);
        opentune::ResampleCorrector corrector;
        corrector.prepare(kSampleRate, kBlockSamples);
        corrector.reset();

        const std::vector<float> in =
            opentune::test::sine(440.0f, kSampleRate, kBlockSamples * kNumBlocks);

        for (int block = 0; block < kNumBlocks; ++block) {
            std::vector<float> out(static_cast<std::size_t>(kBlockSamples), 0.0f);
            corrector.process(in.data() + block * kBlockSamples, out.data(), kBlockSamples, ratio);
            for (float sample : out) {
                CAPTURE(block);
                CHECK_FALSE(std::isnan(sample));
                CHECK_FALSE(std::isinf(sample));
            }
        }
    }
}

TEST_CASE("ResampleCorrector: same input via different block sizes gives the same output "
          "at ratio 0.5") {
    // At pitchRatio <= 1.0 the read position never needs a sample beyond
    // what has already arrived (see header), so -- unlike ratio > 1.0 --
    // the result cannot depend on how the caller chose to chunk the same
    // input into blocks. This is the "block-boundary invariance" test
    // engine/CLAUDE.md requires of every real-time function.
    constexpr int kTotalSamples = 800;
    constexpr float kRatio = 0.5f;

    const std::vector<float> in = opentune::test::sine(220.0f, kSampleRate, kTotalSamples);

    auto runInBlocksOf = [&](int blockSize) {
        opentune::ResampleCorrector corrector;
        corrector.prepare(kSampleRate, 256);
        corrector.reset();

        std::vector<float> out(static_cast<std::size_t>(kTotalSamples), 0.0f);
        int offset = 0;
        while (offset < kTotalSamples) {
            const int n = std::min(blockSize, kTotalSamples - offset);
            corrector.process(in.data() + offset, out.data() + offset, n, kRatio);
            offset += n;
        }
        return out;
    };

    const std::vector<float> outSmallBlocks = runInBlocksOf(64);
    const std::vector<float> outLargeBlocks = runInBlocksOf(256);

    REQUIRE(outSmallBlocks.size() == outLargeBlocks.size());
    for (std::size_t i = 0; i < outSmallBlocks.size(); ++i) {
        CAPTURE(i);
        CHECK(outSmallBlocks[i] == outLargeBlocks[i]);
    }
}

// constitution II / engine/CLAUDE.md: process() and reset() are real-time
// functions and must never allocate, throw, lock, log, or block --
// noexcept is the compiler-enforced half of that contract.
static_assert(noexcept(std::declval<opentune::ResampleCorrector&>().process(nullptr, nullptr, 0,
                                                                            1.0f)),
              "ResampleCorrector::process must be noexcept (audio thread, constitution II)");
static_assert(noexcept(std::declval<opentune::ResampleCorrector&>().reset()),
              "ResampleCorrector::reset must be noexcept (audio thread, constitution II)");
static_assert(noexcept(std::declval<const opentune::ResampleCorrector&>().latencySamples()),
              "ResampleCorrector::latencySamples must be noexcept (audio thread, constitution II)");
