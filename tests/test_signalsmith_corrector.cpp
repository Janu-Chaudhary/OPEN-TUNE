// Tests for opentune::SignalsmithCorrector (T0.14).
//
// Replaces the placeholder registered ahead of T0.13/T0.14 (see the git log).
//
// The whole reason this class exists (docs/decisions/0005) is that
// ResampleCorrector cannot sustain a non-1.0 pitch ratio for more than a few
// seconds -- it dies (collapses to a block-rate staircase) at 0.68-4.84 s
// depending on direction. A test that only checks the first block, or even
// the first second, would pass on the broken corrector too -- that is
// precisely the failure mode a previous review caught in this project (see
// docs/decisions/0005's "Measured on the assembled pipeline" table). So
// every correctness test below runs at least 10 real seconds of audio
// through many small process() calls (256-sample blocks, the block size
// specs.md section 6 calls nominal) and measures pitch from a window near
// the END of the run, never the start.
#include "doctest.h"
#include "opentune/SignalsmithCorrector.h"

#include "support/Signals.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <vector>

namespace {

constexpr double kSampleRate = 48000.0;
constexpr int kBlockSize = 256; // specs.md section 6: block size 256 nominal.

// Cents error between a measured and a reference frequency. See
// engine/CLAUDE.md's Numerics section: cents = 1200 * log2(f/ref).
double centsError(double measuredHz, double referenceHz) {
    return 1200.0 * std::log2(measuredHz / referenceHz);
}

// Estimates frequency by counting upward (negative-to-non-negative) zero
// crossings across `signal` and dividing by elapsed time -- the same coarse
// but simple and easy-to-verify-by-hand measure test_resample_corrector.cpp
// uses, deliberately kept independent of any FFT/autocorrelation machinery
// elsewhere in the engine so this test cannot share a bug with the thing it
// might otherwise be tempted to reuse for verification.
double zeroCrossingFrequency(const float* signal, std::size_t count, double sampleRate) {
    int crossings = 0;
    for (std::size_t i = 1; i < count; ++i) {
        if (signal[i - 1] < 0.0f && signal[i] >= 0.0f) {
            ++crossings;
        }
    }
    const double durationSeconds = static_cast<double>(count) / sampleRate;
    return static_cast<double>(crossings) / durationSeconds;
}

// Runs `totalSamples` of `inputSignal` through `corrector` at a fixed
// `pitchRatio`, `blockSize` samples at a time (the last, possibly short,
// block gets whatever remains) -- i.e. exactly how Engine drives a
// PitchCorrector in real streaming use, never handing it the whole signal
// in one call. Returns the full output buffer, `totalSamples` long.
std::vector<float> runBlocked(opentune::SignalsmithCorrector& corrector,
                              const std::vector<float>& inputSignal, int blockSize,
                              float pitchRatio) {
    const int totalSamples = static_cast<int>(inputSignal.size());
    std::vector<float> output(static_cast<std::size_t>(totalSamples), 0.0f);

    int offset = 0;
    while (offset < totalSamples) {
        const int n = std::min(blockSize, totalSamples - offset);
        corrector.process(inputSignal.data() + offset, output.data() + offset, n, pitchRatio);
        offset += n;
    }
    return output;
}

// --- Allocation-counting infrastructure (constitution II demonstration) ---
//
// The task brief is explicit that Constitution II must be DEMONSTRATED, not
// asserted: "If the library reallocates when the pitch ratio changes
// between blocks ... that is a genuine architectural problem." The only way
// to actually know that, rather than hope it from reading the vendored
// source (see the allocation audit comment in SignalsmithCorrector.cpp), is
// to count real heap operations while process() runs. These global
// operator new/delete overrides do exactly that: when `g_countAllocations`
// is true, every heap (de)allocation anywhere in the process increments a
// counter; the test below arms it only around process() calls (never
// around prepare(), which is allowed -- expected -- to allocate) and
// asserts the counter did not move.
//
// This changes no behaviour when disabled (the default): every path still
// forwards to std::malloc/std::free exactly as ::operator new/delete
// normally would, just with one counter check added. Scoped to this test
// binary; other tests pay a negligible, purely-additive overhead and are
// otherwise unaffected.
std::atomic<bool> g_countAllocations{false};
std::atomic<std::uint64_t> g_allocationCount{0};

} // namespace

void* operator new(std::size_t size) {
    if (g_countAllocations.load(std::memory_order_relaxed)) {
        g_allocationCount.fetch_add(1, std::memory_order_relaxed);
    }
    void* ptr = std::malloc(size);
    if (!ptr) {
        throw std::bad_alloc();
    }
    return ptr;
}

void operator delete(void* ptr) noexcept {
    std::free(ptr);
}

void operator delete(void* ptr, std::size_t) noexcept {
    std::free(ptr);
}

namespace {

// RAII guard: arms allocation counting for its scope, restores it to off on
// exit (including via CHECK failures, which doctest does not unwind past --
// but this is cheap insurance either way).
struct AllocationCountGuard {
    AllocationCountGuard() {
        g_allocationCount.store(0, std::memory_order_relaxed);
        g_countAllocations.store(true, std::memory_order_relaxed);
    }
    ~AllocationCountGuard() { g_countAllocations.store(false, std::memory_order_relaxed); }
};

} // namespace

TEST_CASE("SignalsmithCorrector: sustains a one-semitone UP shift over 10+ seconds "
          "(440 Hz -> 466.16 Hz, measured at the END of the run)") {
    // 11 seconds: comfortably over the 10 s floor the brief requires, with
    // margin so "near the end" can mean the last two full seconds without
    // running into any final-block edge effects.
    constexpr int kTotalSamples = static_cast<int>(11.0 * kSampleRate);
    constexpr float kRatio = 1.0595f; // one semitone up: 2^(1/12).

    opentune::SignalsmithCorrector corrector;
    corrector.prepare(kSampleRate, kBlockSize);
    corrector.reset();

    const std::vector<float> in = opentune::test::sine(440.0f, kSampleRate, kTotalSamples);
    const std::vector<float> out = runBlocked(corrector, in, kBlockSize, kRatio);

    // Measure the last 2 seconds only -- this is the entire point (see file
    // comment): ResampleCorrector was already dead by 4.84 s at the latest;
    // measuring seconds 9-11 proves this implementation is still tracking
    // pitch correctly long after that point, not just at the start.
    constexpr int kMeasureSamples = static_cast<int>(2.0 * kSampleRate);
    const float* measureStart = out.data() + (kTotalSamples - kMeasureSamples);
    const double measuredHz =
        zeroCrossingFrequency(measureStart, static_cast<std::size_t>(kMeasureSamples), kSampleRate);

    INFO("measured Hz near end of 11 s run: ", measuredHz);
    CHECK(std::abs(centsError(measuredHz, 466.16)) < 20.0);
}

TEST_CASE("SignalsmithCorrector: sustains a one-semitone DOWN shift over 10+ seconds "
          "(440 Hz -> 415.30 Hz, measured at the END of the run)") {
    constexpr int kTotalSamples = static_cast<int>(11.0 * kSampleRate);
    constexpr float kRatio = 0.9439f; // one semitone down: 2^(-1/12).

    opentune::SignalsmithCorrector corrector;
    corrector.prepare(kSampleRate, kBlockSize);
    corrector.reset();

    const std::vector<float> in = opentune::test::sine(440.0f, kSampleRate, kTotalSamples);
    const std::vector<float> out = runBlocked(corrector, in, kBlockSize, kRatio);

    constexpr int kMeasureSamples = static_cast<int>(2.0 * kSampleRate);
    const float* measureStart = out.data() + (kTotalSamples - kMeasureSamples);
    const double measuredHz =
        zeroCrossingFrequency(measureStart, static_cast<std::size_t>(kMeasureSamples), kSampleRate);

    INFO("measured Hz near end of 11 s run: ", measuredHz);
    // 440 * 2^(-1/12) = 415.3047 Hz.
    CHECK(std::abs(centsError(measuredHz, 415.3047)) < 20.0);
}

TEST_CASE("SignalsmithCorrector: pitchRatio 1.0 is a close (not bit-identical) "
          "passthrough that preserves pitch") {
    // Unlike ResampleCorrector, this is NOT bit-identical at ratio 1.0: the
    // signal still makes a full round trip through the library's STFT --
    // windowed analysis into a magnitude/phase spectrum and windowed
    // overlap-add resynthesis back into samples -- even when no shift is
    // requested, and that round trip is lossy (finite-precision FFTs,
    // window-function tapering at block edges, phase reconstruction from a
    // magnitude/phase representation). ResampleCorrector could special-case
    // frac == 0.0 to copy samples directly (see its header); a phase
    // vocoder has no equivalent shortcut; unity gain is just another value
    // of `pitchRatio`, run through the same lossy machinery as every other
    // value. So the right assertions here are (a) the output stays close in
    // amplitude to a time-aligned copy of the input, and (b) the pitch it
    // carries is still unmistakably 440 Hz -- not exact float equality.
    constexpr int kTotalSamples = static_cast<int>(3.0 * kSampleRate);

    opentune::SignalsmithCorrector corrector;
    corrector.prepare(kSampleRate, kBlockSize);
    corrector.reset();

    const std::vector<float> in = opentune::test::sine(440.0f, kSampleRate, kTotalSamples);
    const std::vector<float> out = runBlocked(corrector, in, kBlockSize, 1.0f);

    const int latency = corrector.latencySamples();
    REQUIRE(latency > 0); // see the dedicated latency test below; needed here for alignment.

    // Compare a stretch of output against the time-aligned input, skipping
    // the corrector's own reported latency (out[i] corresponds to
    // approximately in[i - latency], not in[i]) and a little extra margin on
    // both ends to stay clear of any transient at stream start and the very
    // last, possibly partial, analysis window.
    const int compareStart = latency + kBlockSize;
    const int compareCount = kTotalSamples - compareStart - kBlockSize;
    REQUIRE(compareCount > static_cast<int>(kSampleRate)); // at least 1 s to compare.

    // `latencySamples()` (inputLatency() + outputLatency(), per the header)
    // is the library's own accounting of its delay, but a 440 Hz sine has a
    // period of only ~109 samples at 48 kHz, so being off by even a few
    // samples from the *true* sample-for-sample alignment turns into a
    // large pointwise error despite the waveform being otherwise
    // essentially unchanged (a phase shift, not a shape change) -- exactly
    // the "an STFT round trip is lossy" case this test's own comment
    // above warns about, just showing up as a lag rather than a distortion.
    // So we search a small window of candidate lags around the reported
    // latency for the one that best aligns the two signals (maximum
    // cross-correlation), then judge closeness at THAT lag. This is the
    // standard way to compare a filtered/reconstructed signal against its
    // source when the filter's exact group delay isn't guaranteed to be an
    // integer number of samples -- it is still a real closeness check (a
    // badly distorted signal cannot fake a high correlation at any lag),
    // it just doesn't require latencySamples() to be exact down to the
    // sample.
    constexpr int kLagSearchRadius = 64;
    int bestLag = 0;
    double bestCorrelation = -1.0;
    for (int lag = -kLagSearchRadius; lag <= kLagSearchRadius; ++lag) {
        double dot = 0.0;
        for (int i = 0; i < compareCount; ++i) {
            const double inputSample = static_cast<double>(in[static_cast<std::size_t>(i)]);
            const double outputSample =
                static_cast<double>(out[static_cast<std::size_t>(compareStart + i + lag)]);
            dot += inputSample * outputSample;
        }
        if (dot > bestCorrelation) {
            bestCorrelation = dot;
            bestLag = lag;
        }
    }
    INFO("best alignment lag relative to reported latency: ", bestLag);

    double sumAbsError = 0.0;
    double sumAbsInput = 0.0;
    for (int i = 0; i < compareCount; ++i) {
        const float inputSample = in[static_cast<std::size_t>(i)];
        const float outputSample = out[static_cast<std::size_t>(compareStart + i + bestLag)];
        sumAbsError +=
            std::abs(static_cast<double>(outputSample) - static_cast<double>(inputSample));
        sumAbsInput += std::abs(static_cast<double>(inputSample));
    }
    // Mean absolute error as a fraction of mean absolute input amplitude,
    // at the best-aligned lag. 15% is generous for a lossy STFT round trip
    // at unity gain (informally measured: a correctly-configured phase
    // vocoder passthrough is much closer than this in practice), while
    // still being tight enough to catch a genuinely broken configuration
    // (e.g. wrong window normalisation, which would produce gross
    // amplitude errors, not a small STFT-rounding one).
    const double relativeError = sumAbsError / sumAbsInput;
    INFO("relative mean-abs error at unity ratio (best-aligned): ", relativeError);
    CHECK(relativeError < 0.15);

    // Pitch must still read as 440 Hz within the same 20-cent tolerance
    // used everywhere else in this file.
    const double measuredHz = zeroCrossingFrequency(
        out.data() + compareStart, static_cast<std::size_t>(compareCount), kSampleRate);
    INFO("measured Hz at unity ratio: ", measuredHz);
    CHECK(std::abs(centsError(measuredHz, 440.0)) < 20.0);
}

TEST_CASE("SignalsmithCorrector: block-size invariance -- the same signal at two "
          "different block sizes produces equivalent audio") {
    // "Equivalent," not bit-identical, is the right standard here (and the
    // brief says so): this corrector's internal STFT analyses input in
    // fixed-size windows (chosen by prepare(), independent of the caller's
    // block size -- see SignalsmithCorrector.cpp's preset comment) and only
    // *decides when to analyse a new window* based on how much input has
    // accumulated so far. Two different block-size splits of the identical
    // input stream reach that "enough has accumulated" threshold after a
    // different number of process() calls, so the exact sample position at
    // which the library performs each internal analysis step can differ by
    // up to a few samples between the two runs -- an artifact of where the
    // caller happened to hand over data, not of the algorithm losing track
    // of the signal. A correct implementation must still converge on the
    // same waveform overall: same pitch, same envelope, no drift.
    constexpr int kTotalSamples = static_cast<int>(4.0 * kSampleRate);
    constexpr float kRatio = 1.0595f;
    constexpr int kBlockSizeA = 256;  // specs.md nominal.
    constexpr int kBlockSizeB = 4001; // deliberately large AND not a divisor
                                      // of kTotalSamples or a multiple of
                                      // kBlockSizeA, so the two runs' internal
                                      // analysis-window boundaries land in
                                      // genuinely different places.

    const std::vector<float> in = opentune::test::sine(440.0f, kSampleRate, kTotalSamples);

    opentune::SignalsmithCorrector correctorA;
    correctorA.prepare(kSampleRate, kBlockSizeA);
    correctorA.reset();
    const std::vector<float> outA = runBlocked(correctorA, in, kBlockSizeA, kRatio);

    opentune::SignalsmithCorrector correctorB;
    correctorB.prepare(kSampleRate, kBlockSizeB);
    correctorB.reset();
    const std::vector<float> outB = runBlocked(correctorB, in, kBlockSizeB, kRatio);

    // Compare a window well clear of both start (latency/transient) and end
    // (final partial-block edge effects) so any real block-size-dependent
    // drift shows up rather than being masked by boundary artifacts.
    const int latency = correctorA.latencySamples();
    const int compareStart = latency + kBlockSizeB; // clear both corrector's latency
                                                    // and the larger block size.
    const int compareCount = kTotalSamples - compareStart - kBlockSizeB;
    REQUIRE(compareCount > static_cast<int>(kSampleRate));

    double sumSquaredDiff = 0.0;
    double sumSquaredA = 0.0;
    for (int i = 0; i < compareCount; ++i) {
        const double a = static_cast<double>(outA[static_cast<std::size_t>(compareStart + i)]);
        const double b = static_cast<double>(outB[static_cast<std::size_t>(compareStart + i)]);
        sumSquaredDiff += (a - b) * (a - b);
        sumSquaredA += a * a;
    }
    // Root-relative-mean-square difference: normalises out signal
    // amplitude, so this tolerance means "10% of the signal's own RMS
    // level," not an absolute number tied to this particular amplitude.
    const double relativeRmsDiff = std::sqrt(sumSquaredDiff / sumSquaredA);
    INFO("relative RMS difference between block sizes 256 and 4001: ", relativeRmsDiff);
    CHECK(relativeRmsDiff < 0.10);

    // Both must still land on the same shifted pitch.
    const double hzA = zeroCrossingFrequency(outA.data() + compareStart,
                                             static_cast<std::size_t>(compareCount), kSampleRate);
    const double hzB = zeroCrossingFrequency(outB.data() + compareStart,
                                             static_cast<std::size_t>(compareCount), kSampleRate);
    INFO("Hz at block size 256: ", hzA, " / Hz at block size 4001: ", hzB);
    CHECK(std::abs(centsError(hzA, 466.16)) < 20.0);
    CHECK(std::abs(centsError(hzB, 466.16)) < 20.0);
}

TEST_CASE("SignalsmithCorrector: latencySamples() reports the library's real, "
          "non-zero latency") {
    opentune::SignalsmithCorrector corrector;
    corrector.prepare(kSampleRate, kBlockSize);

    // Unlike ResampleCorrector (always exactly 0 -- see its header), a
    // phase vocoder cannot produce a shifted sample without first analysing
    // a whole window of input, so this must be strictly positive. See
    // SignalsmithCorrector.cpp for the exact preset this measures and the
    // number it produces (recorded in the T0.13/T0.14 report).
    const int latency = corrector.latencySamples();
    INFO("measured latencySamples(): ", latency);
    CHECK(latency > 0);
}

TEST_CASE("SignalsmithCorrector: process() performs zero heap allocations across "
          "10+ seconds, both pitch directions, with the ratio changing every block "
          "(constitution II, demonstrated not asserted)") {
    // This is the test the T0.13/T0.14 brief calls for explicitly: "If the
    // library reallocates when the pitch ratio changes between blocks --
    // which for us happens every block -- that is a genuine architectural
    // problem." So this test does exactly that -- changes the ratio on
    // every single 256-sample block, for the full 10+ second run, in both
    // directions -- while counting every heap (de)allocation anywhere in
    // the process.
    constexpr int kTotalSamples = static_cast<int>(10.0 * kSampleRate);

    opentune::SignalsmithCorrector corrector;
    // prepare() is expected (and allowed, per the PitchCorrector interface)
    // to allocate -- it is not real-time code -- so it runs OUTSIDE the
    // AllocationCountGuard's scope below.
    corrector.prepare(kSampleRate, kBlockSize);
    corrector.reset();

    const std::vector<float> in = opentune::test::sine(440.0f, kSampleRate, kTotalSamples);
    std::vector<float> out(static_cast<std::size_t>(kTotalSamples), 0.0f);

    {
        AllocationCountGuard guard;

        int offset = 0;
        bool up = true;
        while (offset < kTotalSamples) {
            const int n = std::min(kBlockSize, kTotalSamples - offset);
            // Alternate every block between one semitone up and one
            // semitone down: the worst case the brief describes ("the
            // ratio moves per block") and the most likely way a
            // ratio-triggered reallocation, if one existed, would show up.
            const float ratio = up ? 1.0595f : 0.9439f;
            up = !up;
            corrector.process(in.data() + offset, out.data() + offset, n, ratio);
            corrector.reset(); // also exercised here: reset() must not allocate either.
            offset += n;
        }
    }

    const std::uint64_t allocations = g_allocationCount.load(std::memory_order_relaxed);
    INFO("heap allocations observed during process()/reset() across 10 s, "
         "per-block ratio changes: ",
         allocations);
    CHECK(allocations == 0);
}
