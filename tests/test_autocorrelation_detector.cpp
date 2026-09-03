#include "doctest.h"
#include "opentune/AutocorrelationDetector.h"

#include "support/Signals.h"

#include <cmath>
#include <utility>
#include <vector>

// T0.4 done-criteria (tasks.md): detects 110/220/440/880 Hz sines within
// +/-20 cents; reports voiced=false for silence. engine/CLAUDE.md's "three
// tests minimum" also requires a block-boundary test. See
// .superpowers/sdd/tasks/task-T0.4-brief.md for the four required
// categories, all present below: accuracy (four frequencies), silence,
// pre-fill behaviour, block-size invariance.

namespace {

constexpr double kSampleRate = 48000.0;
constexpr int kMaxBlockSize = 256;

// Signal length used by tests that need the detector to "settle" (its
// ring buffer to fill and then hold several windows' worth of consistent
// signal). At 48 kHz this detector's buffer is on the order of ~2200
// samples (prepare()'s derivation from the 65-1100 Hz detectable range);
// 0.2s = 9600 samples is comfortably more than that (over 4x), while
// staying far short of a full second -- this detector's O(lags * window)
// search is deliberately expensive (see AutocorrelationDetector.h), so
// keeping test signals no longer than necessary keeps the suite fast.
constexpr int kSettleSignalSamples = 9600;

// Cents error between a measured and reference frequency
// (engine/CLAUDE.md Numerics: cents = 1200 * log2(f_measured / f_reference)).
// Used to assert accuracy in musically-meaningful units rather than raw Hz,
// since a fixed Hz tolerance means something different at 110 Hz than at
// 880 Hz.
double centsError(double measuredHz, double referenceHz) {
    return 1200.0 * std::log2(measuredHz / referenceHz);
}

// Feeds an entire signal through a detector in fixed-size blocks (the last,
// possibly short, block is fed as-is) and returns the estimate from the
// *final* block -- i.e. the detector's answer once its ring buffer has
// seen the whole signal ("settled").
opentune::PitchEstimate runToSettledEstimate(opentune::AutocorrelationDetector& detector,
                                             const std::vector<float>& signal, int blockSize) {
    opentune::PitchEstimate lastEstimate{0.0f, 0.0f, false};
    std::size_t offset = 0;
    while (offset < signal.size()) {
        const int n = static_cast<int>(
            std::min<std::size_t>(static_cast<std::size_t>(blockSize), signal.size() - offset));
        lastEstimate = detector.process(signal.data() + offset, n);
        offset += static_cast<std::size_t>(n);
    }
    return lastEstimate;
}

} // namespace

TEST_CASE("AutocorrelationDetector detects 110/220/440/880 Hz sines within +/-20 cents") {
    // 70 Hz and 1090 Hz are regression cases for a lag-range-boundary bug
    // (review findings round 1, Important 2): prepare() used to round lag
    // bounds to the nearest sample rather than widening them, and the
    // first-peak search excluded the array endpoints outright, so
    // 1090.9 Hz (period exactly at the old rounded minLag of 44 samples at
    // 48 kHz) fell just outside the searchable/selectable range and was
    // detected an octave low (~545 Hz) instead. 70 Hz exercises the
    // equivalent boundary near kMinFrequencyHz / maxLag.
    const float testFrequencies[] = {70.0f, 110.0f, 220.0f, 440.0f, 880.0f, 1090.0f};

    for (float hz : testFrequencies) {
        CAPTURE(hz);

        opentune::AutocorrelationDetector detector;
        detector.prepare(kSampleRate, kMaxBlockSize);

        // kSettleSignalSamples is comfortably more than enough to fill the
        // detector's analysis window at any of these frequencies.
        const std::vector<float> signal =
            opentune::test::sine(hz, kSampleRate, kSettleSignalSamples);

        const opentune::PitchEstimate estimate =
            runToSettledEstimate(detector, signal, kMaxBlockSize);

        REQUIRE(estimate.voiced == true);
        const double cents = centsError(static_cast<double>(estimate.frequencyHz), hz);
        CAPTURE(cents);
        CHECK(std::abs(cents) <= 20.0);
    }
}

TEST_CASE("AutocorrelationDetector reports voiced=false for silence") {
    opentune::AutocorrelationDetector detector;
    detector.prepare(kSampleRate, kMaxBlockSize);

    // Plenty of silence to fill the analysis window several times over.
    const std::vector<float> signal = opentune::test::silence(kSettleSignalSamples);

    const opentune::PitchEstimate estimate = runToSettledEstimate(detector, signal, kMaxBlockSize);

    CHECK(estimate.voiced == false);
    CHECK(estimate.frequencyHz == 0.0f);
}

TEST_CASE("AutocorrelationDetector reports voiced=false for white noise") {
    // Review findings round 1, Important 3: the silence test returns early
    // at the RMS gate and never exercises the correlation-based voicing
    // rejection or the global-max fallback path. Full-scale white noise
    // passes the RMS gate (it has real energy) but has no lag anywhere
    // near a perfect self-match, so it must be rejected by the normalised
    // correlation threshold instead.
    opentune::AutocorrelationDetector detector;
    detector.prepare(kSampleRate, kMaxBlockSize);

    const std::vector<float> signal =
        opentune::test::whiteNoise(kSettleSignalSamples, /*seed=*/12345u);

    const opentune::PitchEstimate estimate = runToSettledEstimate(detector, signal, kMaxBlockSize);

    CHECK(estimate.voiced == false);
    CHECK(estimate.frequencyHz == 0.0f);
}

TEST_CASE("AutocorrelationDetector reports voiced=false until its analysis window fills") {
    opentune::AutocorrelationDetector detector;
    detector.prepare(kSampleRate, kMaxBlockSize);

    // A single small block is far short of the ~samples-per-window the
    // detector needs (per the brief, on the order of ~2048 samples at
    // 48 kHz) -- the very first call must not analyse a mostly-uninitialised
    // buffer.
    const std::vector<float> firstBlock = opentune::test::sine(440.0f, kSampleRate, kMaxBlockSize);
    const opentune::PitchEstimate estimate = detector.process(firstBlock.data(), kMaxBlockSize);

    CHECK(estimate.voiced == false);
    CHECK(estimate.frequencyHz == 0.0f);
    CHECK(estimate.confidence == 0.0f);
}

TEST_CASE("AutocorrelationDetector: same signal via different block sizes settles to the same "
          "estimate") {
    // engine/CLAUDE.md "Tests for this directory": every real-time function
    // needs a block-boundary test -- the same input split into different
    // block sizes must produce identical output once settled. Because this
    // detector's estimate depends only on the most recent
    // m_buffer.size() samples (not on how they arrived), feeding the same
    // signal through 64-sample and 256-sample blocks must settle to the
    // same estimate.
    //
    // Review findings round 1, Minor 7: kSettleSignalSamples (9600) divides
    // evenly by both 64 and 256, so both runs would end on a byte-identical
    // final window regardless of whether this invariance genuinely held --
    // a length one sample longer forces the final blocks of each run to
    // land at different offsets into the ring buffer's shift-and-append
    // sequence, so the assertion actually exercises alignment-independence
    // rather than being structurally guaranteed by the chosen length.
    const std::vector<float> signal =
        opentune::test::sine(440.0f, kSampleRate, kSettleSignalSamples + 1);

    opentune::AutocorrelationDetector detectorSmallBlocks;
    detectorSmallBlocks.prepare(kSampleRate, kMaxBlockSize);
    const opentune::PitchEstimate estimateSmallBlocks =
        runToSettledEstimate(detectorSmallBlocks, signal, 64);

    opentune::AutocorrelationDetector detectorLargeBlocks;
    detectorLargeBlocks.prepare(kSampleRate, kMaxBlockSize);
    const opentune::PitchEstimate estimateLargeBlocks =
        runToSettledEstimate(detectorLargeBlocks, signal, 256);

    REQUIRE(estimateSmallBlocks.voiced == true);
    REQUIRE(estimateLargeBlocks.voiced == true);
    CHECK(estimateSmallBlocks.frequencyHz == doctest::Approx(estimateLargeBlocks.frequencyHz));
}

// constitution II / engine/CLAUDE.md: process() and reset() are real-time
// functions and must never allocate, throw, lock, log, or block --
// noexcept is the compiler-enforced half of that contract.
static_assert(noexcept(std::declval<opentune::AutocorrelationDetector&>().process(nullptr, 0)),
              "AutocorrelationDetector::process must be noexcept (audio thread, constitution II)");
static_assert(noexcept(std::declval<opentune::AutocorrelationDetector&>().reset()),
              "AutocorrelationDetector::reset must be noexcept (audio thread, constitution II)");
