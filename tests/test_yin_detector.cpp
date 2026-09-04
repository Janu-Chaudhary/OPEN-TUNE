// Tests for YinDetector -- the Stage 1 pitch detector implementing YIN
// (de Cheveigne & Kawahara 2002). Built up in the same five steps the
// algorithm itself has (T1.3 - T1.7), so each test block below names the
// step it drove.
//
// All signals are generated locally (tests/support/Signals.h plus the
// harmonic-stack helper at the bottom of this preamble): this file
// deliberately does not depend on the shared octave/pitch-error helpers
// being written concurrently.
#include "doctest.h"

#include "opentune/AutocorrelationDetector.h"
#include "opentune/PitchDetector.h"
#include "opentune/YinDetector.h"
#include "support/Signals.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace {

using opentune::PitchEstimate;
using opentune::YinDetector;

// The engine's nominal rate (specs.md section 6). Passed explicitly
// everywhere so nothing in the engine has to hardcode it.
constexpr double kSampleRate = 48000.0;

// The engine's nominal block size (specs.md section 6): 256 samples,
// 5.33 ms at 48 kHz.
constexpr int kBlockSize = 256;

// Feeds `signal` through `detector` in `kBlockSize`-sample blocks and
// returns the estimate from the final block. A detector needs a full
// analysis window (~2200 samples here) before it can say anything, so
// tests must push at least that much audio through before reading an
// answer -- one block is never enough.
PitchEstimate feedBlocks(opentune::PitchDetector& detector, const std::vector<float>& signal) {
    PitchEstimate estimate{0.0f, 0.0f, false};
    int offset = 0;
    const int total = static_cast<int>(signal.size());
    while (offset < total) {
        const int n = std::min(kBlockSize, total - offset);
        estimate = detector.process(signal.data() + static_cast<std::size_t>(offset), n);
        offset += n;
    }
    return estimate;
}

// Enough audio to fill the analysis window several times over and still
// leave the final block reading a window made entirely of real signal.
// The window is ~2218 samples at 48 kHz, so 4096 clears it comfortably.
// Deliberately not longer: every block past the fill point re-runs the
// full O(W * L) analysis, so a longer signal buys nothing but test time.
constexpr int kTestSamples = 4096;

} // namespace

// ---------------------------------------------------------------------------
// T1.3 -- the difference function
// ---------------------------------------------------------------------------
//
// YIN's first step replaces autocorrelation's "how SIMILAR is the signal to
// itself shifted by tau?" with "how DIFFERENT is it?":
//
//     d(tau) = sum over j of (x[j] - x[j + tau])^2
//
// A perfectly periodic signal is identical to itself one period later, so
// d(tau) drops to (near) zero at tau = the period -- and at every multiple
// of it. d(0) is exactly zero by construction: a signal shifted by nothing
// is itself.
TEST_CASE("YIN difference function is exactly zero at lag zero") {
    YinDetector detector;
    detector.prepare(kSampleRate, kBlockSize);

    const std::vector<float> signal = opentune::test::sine(440.0f, kSampleRate, kTestSamples);
    feedBlocks(detector, signal);

    const std::vector<double>& d = detector.differenceFunction();
    REQUIRE(d.size() > 1);
    CHECK(d[0] == doctest::Approx(0.0));
}

TEST_CASE("YIN difference function dips at the true period of a sine") {
    YinDetector detector;
    detector.prepare(kSampleRate, kBlockSize);

    const std::vector<float> signal = opentune::test::sine(440.0f, kSampleRate, kTestSamples);
    feedBlocks(detector, signal);

    const std::vector<double>& d = detector.differenceFunction();
    const int minLag = detector.minLagSamples();
    const int maxLag = detector.maxLagSamples();
    REQUIRE(static_cast<int>(d.size()) > maxLag);

    // 48000 / 440 = 109.09 samples per period.
    const double expectedPeriod = kSampleRate / 440.0;

    int argmin = minLag;
    for (int tau = minLag; tau <= maxLag; ++tau) {
        if (d[static_cast<std::size_t>(tau)] < d[static_cast<std::size_t>(argmin)]) {
            argmin = tau;
        }
    }

    // Within one sample of the true period: the difference function is
    // evaluated only at integer lags, so it cannot do better than that
    // until parabolic interpolation is added (T1.6).
    CHECK(std::abs(static_cast<double>(argmin) - expectedPeriod) <= 1.0);

    // ...and the dip is deep: at the period the signal really does match
    // itself, so d there is a tiny fraction of its typical magnitude
    // (compare against the half-period lag, where a sine is maximally
    // *un*like itself -- it is exactly inverted).
    const int halfPeriodLag = static_cast<int>(expectedPeriod / 2.0);
    CHECK(d[static_cast<std::size_t>(argmin)] < 0.01 * d[static_cast<std::size_t>(halfPeriodLag)]);
}

// ---------------------------------------------------------------------------
// T1.4 -- cumulative mean normalised difference (the octave-error step)
// ---------------------------------------------------------------------------
//
//     d'(0) = 1,   d'(tau) = d(tau) / [ (1/tau) * sum_{j=1..tau} d(j) ]
//
// Each difference is divided by the running mean of every difference at a
// shorter lag. Two things follow, and both are tested below.
//
// First, d'(0) is DEFINED as 1 rather than computed, because d(0) is
// exactly 0 and would otherwise be an unbeatable false minimum.
//
// Second -- the property that kills the octave error -- short lags stop
// being cheap. On the rising slope out of tau = 0, every d(tau) is larger
// than the mean of everything before it, so d' >= 1 there by construction:
// the whole region of small raw differences near lag 0, and in particular
// every lag at a fraction of the true period, is disqualified without any
// threshold having to be tuned. Meanwhile the running mean keeps growing
// with tau, so by lag 2P the denominator has swallowed an extra period of
// large differences and an identical dip is normalised less generously
// than the one at P was. The function is biased toward the SHORTEST true
// period, which is the correct one.
TEST_CASE("YIN cumulative mean normalised difference is 1 at lag zero") {
    YinDetector detector;
    detector.prepare(kSampleRate, kBlockSize);

    const std::vector<float> signal = opentune::test::sine(440.0f, kSampleRate, kTestSamples);
    feedBlocks(detector, signal);

    const std::vector<double>& cmnd = detector.cumulativeMeanNormalizedDifference();
    REQUIRE(cmnd.size() > 1);
    CHECK(cmnd[0] == doctest::Approx(1.0));
}

TEST_CASE("YIN cumulative mean normalised difference matches its definition") {
    YinDetector detector;
    detector.prepare(kSampleRate, kBlockSize);

    const std::vector<float> signal = opentune::test::sine(220.0f, kSampleRate, kTestSamples);
    feedBlocks(detector, signal);

    const std::vector<double>& d = detector.differenceFunction();
    const std::vector<double>& cmnd = detector.cumulativeMeanNormalizedDifference();
    REQUIRE(d.size() == cmnd.size());

    // Recompute the definition independently and compare, lag by lag.
    double running = 0.0;
    for (std::size_t tau = 1; tau < d.size(); ++tau) {
        running += d[tau];
        const double expected = d[tau] * static_cast<double>(tau) / running;
        REQUIRE(cmnd[tau] == doctest::Approx(expected).epsilon(1e-9));
    }
}

TEST_CASE("YIN normalisation makes short lags expensive rather than cheap") {
    YinDetector detector;
    detector.prepare(kSampleRate, kBlockSize);

    // 70 Hz -> a period of 48000 / 70 = 685.7 samples. Every lag below
    // half of that is on the rising slope of d. A low pitch is the honest
    // signal for this test: the shortest searchable lag (43 samples, the
    // 1100 Hz end of the range) is then only ~6% of a period, which is
    // exactly the "barely shifted, barely different" region where a raw
    // difference function is at its most misleading.
    const std::vector<float> signal = opentune::test::sine(70.0f, kSampleRate, kTestSamples);
    feedBlocks(detector, signal);

    const std::vector<double>& d = detector.differenceFunction();
    const std::vector<double>& cmnd = detector.cumulativeMeanNormalizedDifference();
    const int minLag = detector.minLagSamples();
    const int period = static_cast<int>(kSampleRate / 70.0);

    // The raw difference function really is temptingly small at short
    // lags: at the shortest searchable lag it is a small fraction of its
    // value at the true half-period, where the sine is inverted and the
    // differences are largest. Left to itself, that slope is exactly the
    // kind of spurious minimum a naive search can fall into.
    const double dAtMinLag = d[static_cast<std::size_t>(minLag)];
    const double dAtHalfPeriod = d[static_cast<std::size_t>(period / 2)];
    REQUIRE(dAtHalfPeriod > 0.0);
    CHECK(dAtMinLag < 0.2 * dAtHalfPeriod);

    // After normalisation that temptation is gone: everywhere on the
    // rising slope, d' is at or above 1 -- no better than the "no
    // periodicity at all" value.
    for (int tau = 1; tau <= period / 2; ++tau) {
        REQUIRE(cmnd[static_cast<std::size_t>(tau)] >= 1.0);
    }

    // ...while the true period still dips far below it.
    CHECK(cmnd[static_cast<std::size_t>(period)] < 0.1);
}

TEST_CASE("YIN normalisation alone still ties a period with its double") {
    YinDetector detector;
    detector.prepare(kSampleRate, kBlockSize);

    // 300 Hz: period exactly 160 samples, doubled period 320 -- both well
    // inside the 65-1100 Hz lag range, so both are lags a detector could
    // legitimately return. One of them is an octave error.
    const std::vector<float> signal = opentune::test::sine(300.0f, kSampleRate, kTestSamples);
    feedBlocks(detector, signal);

    const std::vector<double>& d = detector.differenceFunction();
    const std::vector<double>& cmnd = detector.cumulativeMeanNormalizedDifference();
    const int period = 160;
    const int doubledPeriod = 320;

    // The ambiguity, in two lines: the raw difference function is exactly
    // as happy at twice the period as at the period itself. Both dips are
    // a negligible fraction of the function's range, so nothing in d alone
    // -- and nothing in the autocorrelation it is equivalent to -- says
    // which of the two is the fundamental.
    const double range = d[static_cast<std::size_t>(period / 2)];
    REQUIRE(range > 0.0);
    CHECK(d[static_cast<std::size_t>(period)] / range < 0.01);
    CHECK(d[static_cast<std::size_t>(doubledPeriod)] / range < 0.01);

    // And this is the honest limit of step 2, asserted rather than glossed
    // over: normalisation does NOT separate them either. For a stationary
    // periodic signal the running mean has already plateaued by the first
    // period -- averaging over one more identical cycle changes nothing --
    // so P and 2P are divided by the same denominator and both land at
    // essentially zero. Step 2's real achievement is elsewhere (the test
    // above: the rising slope, and every lag at a fraction of the period,
    // is pushed to >= 1), and it is what makes a single fixed threshold
    // meaningful across signals.
    //
    // Breaking THIS tie is step 3's job (T1.5) and it does it by taking
    // the FIRST lag under that threshold rather than the deepest -- a rule
    // that is only well defined because step 2 put every lag on a common
    // scale. The detector-level octave tests further down show it working.
    CHECK(cmnd[static_cast<std::size_t>(period)] < 0.01);
    CHECK(cmnd[static_cast<std::size_t>(doubledPeriod)] < 0.01);
}

// ---------------------------------------------------------------------------
// T1.5 -- absolute threshold and best-candidate selection
// ---------------------------------------------------------------------------
//
// With every lag on a common 0..~1 scale, YIN walks lags from short to
// long and takes the FIRST one that dips below a fixed absolute threshold
// (0.1 in the paper), descending to the bottom of that dip. Not the global
// minimum -- the global minimum is a coin flip between P, 2P and 3P, all of
// which score alike. "First" means "shortest period", so period doubling
// loses by construction.
//
// The tests below check the integer-lag answer only: no interpolation yet,
// so the tolerance here is loose (the quantisation is +/-39 cents at the
// top of the range). T1.6 tightens it to the +/-5 cents AC1 asks for.

namespace {

// A harmonic stack: partials at k * f0 for k in [kFirst, kLast], with
// amplitude (oddScale or evenScale) / k depending on whether k is odd or
// even. The 1/k rolloff is roughly a natural voiced tone's spectral slope.
// The result is scaled to 0.8 peak so it stays inside the nominal +/-1.0.
//
// Two settings of this generator are the octave traps used below.
//
//   kFirst = 2  -- the fundamental is REMOVED entirely. There is no energy
//     whatsoever at f0, yet the signal's true period is still 1/f0: that is
//     the shortest interval after which the whole sum of partials repeats.
//     A detector that reasons from "which frequency is strongest?" answers
//     2*f0 and is an octave sharp; one that reasons from the period answers
//     f0. The ear does the latter -- this is the "missing fundamental" or
//     virtual pitch effect, and it is why a small radio with no bass
//     response still plays in the right key.
//
//   oddScale well below evenScale -- the fundamental is present but weak
//     relative to its first harmonic, the common real-voice case named in
//     docs/decisions/0003-first-peak-autocorrelation.md (telephone band,
//     belted high notes, male vowels where H2 outweighs H1). At a lag of
//     half a period, the even partials line straight back up while only the
//     weak odd ones fight it, so the signal looks *almost* periodic an
//     octave up. That near-miss is what a plain correlation-strength
//     threshold cannot tell from the real thing.
std::vector<float> harmonicStack(double fundamentalHz, double sampleRate, int numSamples,
                                 int kFirst, int kLast, double oddScale, double evenScale) {
    std::vector<float> out(static_cast<std::size_t>(numSamples), 0.0f);
    double peak = 0.0;
    for (int i = 0; i < numSamples; ++i) {
        double value = 0.0;
        for (int k = kFirst; k <= kLast; ++k) {
            const double amplitude = ((k % 2) != 0 ? oddScale : evenScale) / static_cast<double>(k);
            const double hz = fundamentalHz * static_cast<double>(k);
            value += amplitude *
                     std::sin(opentune::test::kTwoPi * hz * static_cast<double>(i) / sampleRate);
        }
        out[static_cast<std::size_t>(i)] = static_cast<float>(value);
        peak = std::max(peak, std::abs(value));
    }
    if (peak > 0.0) {
        const float scale = static_cast<float>(0.8 / peak);
        for (float& sample : out) {
            sample *= scale;
        }
    }
    return out;
}

// Cents between a measured and a reference frequency:
// 1200 * log2(f_measured / f_reference) (constraints.md, specs.md section 6).
// Defined locally on purpose -- see the file preamble.
double centsError(double measuredHz, double referenceHz) {
    return 1200.0 * std::log2(measuredHz / referenceHz);
}

// The frequencies AC1 is measured over: the ends of the declared 65-1100 Hz
// range, a spread of musical pitches across it, and a few deliberately
// non-note frequencies so nothing can pass by landing on a lucky lag.
const double kSweepFrequenciesHz[] = {65.0,   73.42,  82.41,  98.0,   110.0,  123.47, 146.83,
                                      164.81, 196.0,  220.0,  261.63, 293.66, 329.63, 392.0,
                                      440.0,  493.88, 523.25, 587.33, 659.25, 700.0,  783.99,
                                      880.0,  932.33, 987.77, 1046.5, 1080.0, 1100.0};

} // namespace

TEST_CASE("YIN reports a frequency for a sine, at integer-lag accuracy") {
    for (const double hz : kSweepFrequenciesHz) {
        YinDetector detector;
        detector.prepare(kSampleRate, kBlockSize);

        const std::vector<float> signal =
            opentune::test::sine(static_cast<float>(hz), kSampleRate, kTestSamples);
        const PitchEstimate estimate = feedBlocks(detector, signal);

        INFO("frequency ", hz, " Hz -> ", estimate.frequencyHz, " Hz");
        CHECK(estimate.voiced);
        REQUIRE(estimate.frequencyHz > 0.0f);
        // +/-45 cents: one integer lag at the 1100 Hz end of the range is
        // worth ~39 cents, so this is the best an un-interpolated lag can
        // do. T1.6 is what takes this to +/-5.
        CHECK(std::abs(centsError(static_cast<double>(estimate.frequencyHz), hz)) < 45.0);
    }
}

TEST_CASE("YIN survives weak-fundamental signals that send autocorrelation an octave up") {
    // The comparison that justifies replacing the Stage 0 detector, on the
    // failure mode docs/decisions/0003 predicts for it.
    //
    // Signal: harmonics 1..8 at 1/k, with every ODD harmonic scaled down by
    // a further factor of 5 (~14 dB). The fundamental is therefore present
    // but much weaker than the second harmonic. At a lag of half a period
    // the even harmonics realign exactly and only the weak odd ones
    // disagree, so the plain normalised correlation there reaches ~0.77 --
    // comfortably past AutocorrelationDetector's 0.6 qualifying threshold,
    // and it is reached FIRST, so that detector returns an octave too high.
    //
    // YIN sees the same near-match, but on the normalised difference scale
    // it comes out at ~0.23, well above the 0.1 absolute threshold, so the
    // half-period lag never qualifies and the walk continues to the true
    // period. The margin is what the normalisation buys: the two functions
    // are looking at the same near-miss and only one of them has a scale on
    // which "near" is distinguishable from "right".
    const double kOddScale = 0.2;
    const double kEvenScale = 1.0;

    // Kept below 550 Hz so the octave-up answer (2 * f0) is itself still
    // inside the 65-1100 Hz search range -- above that the baseline gets
    // these right for the uninteresting reason that its only wrong answer
    // is out of range.
    const double fundamentals[] = {65.0,  82.41,  110.0, 146.83, 196.0,
                                   220.0, 293.66, 392.0, 440.0,  523.25};

    int yinOctaveErrors = 0;
    int autocorrelationOctaveErrors = 0;

    for (const double f0 : fundamentals) {
        const std::vector<float> signal =
            harmonicStack(f0, kSampleRate, kTestSamples, 1, 8, kOddScale, kEvenScale);

        YinDetector yin;
        yin.prepare(kSampleRate, kBlockSize);
        const PitchEstimate yinEstimate = feedBlocks(yin, signal);

        opentune::AutocorrelationDetector autocorrelation;
        autocorrelation.prepare(kSampleRate, kBlockSize);
        const PitchEstimate autocorrelationEstimate = feedBlocks(autocorrelation, signal);

        INFO("f0 ", f0, " Hz: yin ", yinEstimate.frequencyHz, " Hz, autocorrelation ",
             autocorrelationEstimate.frequencyHz, " Hz");

        // An octave error is a reading a factor of two or more off. 1200
        // cents is exactly one octave; past 600 a reading is closer to the
        // octave than to the truth.
        const double yinCents = yinEstimate.frequencyHz > 0.0f
                                    ? centsError(static_cast<double>(yinEstimate.frequencyHz), f0)
                                    : 0.0;
        const double autocorrelationCents =
            autocorrelationEstimate.frequencyHz > 0.0f
                ? centsError(static_cast<double>(autocorrelationEstimate.frequencyHz), f0)
                : 0.0;
        if (std::abs(yinCents) > 600.0) {
            ++yinOctaveErrors;
        }
        if (std::abs(autocorrelationCents) > 600.0) {
            ++autocorrelationOctaveErrors;
        }

        CHECK(yinEstimate.voiced);
        CHECK(std::abs(yinCents) < 45.0);
    }

    const int caseCount = static_cast<int>(sizeof(fundamentals) / sizeof(fundamentals[0]));
    CHECK(yinOctaveErrors == 0);
    // The baseline is expected to fail every one of these. If it ever stops,
    // this comparison has lost its meaning and should be revisited rather
    // than quietly relaxed.
    CHECK(autocorrelationOctaveErrors == caseCount);
}

TEST_CASE("YIN hears the missing fundamental") {
    // The textbook virtual-pitch case: no energy at f0 at all, partials at
    // 2*f0 .. 5*f0 only. The true period is still 1/f0 and YIN must report
    // it. (AutocorrelationDetector happens to pass this one too -- with all
    // partials at full 1/k weight, the half-period correlation is dragged
    // down by the odd ones. It is included because it is the classic
    // statement of the problem, not as a baseline comparison.)
    const double fundamentals[] = {110.0, 146.83, 220.0, 293.66};
    for (const double f0 : fundamentals) {
        const std::vector<float> signal =
            harmonicStack(f0, kSampleRate, kTestSamples, 2, 5, 1.0, 1.0);

        YinDetector yin;
        yin.prepare(kSampleRate, kBlockSize);
        const PitchEstimate estimate = feedBlocks(yin, signal);

        INFO("f0 ", f0, " Hz -> ", estimate.frequencyHz, " Hz");
        CHECK(estimate.voiced);
        REQUIRE(estimate.frequencyHz > 0.0f);
        CHECK(std::abs(centsError(static_cast<double>(estimate.frequencyHz), f0)) < 45.0);
    }
}

// ---------------------------------------------------------------------------
// T1.6 -- parabolic interpolation for sub-sample precision
// ---------------------------------------------------------------------------
//
// Integer lags quantise the answer, and they quantise it worst where it
// hurts most. A period is sampleRate/f samples long, so at 65 Hz a period
// is 738 samples and being one sample out is 2.3 cents -- inaudible. At
// 1100 Hz a period is only 43.6 samples and one sample out is 39 cents --
// a quarter of a semitone, plainly wrong. Since a true period is almost
// never a whole number of samples, that error is the normal case, not the
// worst case.
//
// The fix is to stop treating d' as a list of values and start treating it
// as a sampled curve. Near its minimum the curve is locally parabolic, so
// fitting a parabola through the chosen lag and its two neighbours and
// taking the vertex of that parabola recovers a fractional lag. This is
// YIN step 5, and it is what takes the error from tens of cents to a few.
//
// This is AC1 (specs.md section 9): synthetic sine, 65-1100 Hz, +/-5 cents.
TEST_CASE("AC1: YIN detects synthetic sines from 65 to 1100 Hz within 5 cents") {
    double worstAbsCents = 0.0;
    double worstAtHz = 0.0;

    for (const double hz : kSweepFrequenciesHz) {
        YinDetector detector;
        detector.prepare(kSampleRate, kBlockSize);

        const std::vector<float> signal =
            opentune::test::sine(static_cast<float>(hz), kSampleRate, kTestSamples);
        const PitchEstimate estimate = feedBlocks(detector, signal);

        REQUIRE(estimate.voiced);
        REQUIRE(estimate.frequencyHz > 0.0f);
        const double cents = centsError(static_cast<double>(estimate.frequencyHz), hz);
        if (std::abs(cents) > worstAbsCents) {
            worstAbsCents = std::abs(cents);
            worstAtHz = hz;
        }
        INFO("frequency ", hz, " Hz -> ", estimate.frequencyHz, " Hz (", cents, " cents)");
        CHECK(std::abs(cents) < 5.0);
    }

    INFO("worst error ", worstAbsCents, " cents at ", worstAtHz, " Hz");
    CHECK(worstAbsCents < 5.0);
}

TEST_CASE("YIN reports a fractional period, not a whole number of samples") {
    // A direct check that interpolation is actually happening rather than
    // the sweep above passing by luck. 673 Hz has a period of 71.32
    // samples: an integer-lag detector can only answer 48000/71 = 676.06
    // or 48000/72 = 666.67 Hz, both more than 5 cents out. Anything
    // between them can only have come from a fractional lag.
    const double hz = 673.0;
    YinDetector detector;
    detector.prepare(kSampleRate, kBlockSize);

    const std::vector<float> signal =
        opentune::test::sine(static_cast<float>(hz), kSampleRate, kTestSamples);
    const PitchEstimate estimate = feedBlocks(detector, signal);

    REQUIRE(estimate.voiced);
    const double reportedLag = kSampleRate / static_cast<double>(estimate.frequencyHz);
    INFO("reported lag ", reportedLag, " samples");
    CHECK(std::abs(reportedLag - std::round(reportedLag)) > 0.01);
    CHECK(std::abs(centsError(static_cast<double>(estimate.frequencyHz), hz)) < 5.0);
}

// ---------------------------------------------------------------------------
// T1.7 -- voiced/unvoiced from the aperiodicity measure (AC6)
// ---------------------------------------------------------------------------
//
// Steps 1-4 always name a lag: the fallback picks the shallowest dip even
// when nothing looks periodic at all. Something has to decide whether that
// lag means anything, and YIN already computed the number that answers it.
//
// d' at the chosen lag IS the aperiodicity. It needs no separate
// statistic, no loudness rule, no tuning against a corpus: 0 means the
// signal repeats perfectly at that lag, ~1 means it is no more self-similar
// there than at any random lag. Silence and white noise never produce a low
// value -- silence because every difference is zero and the guarded ratio
// is defined as 1, noise because it genuinely has no period -- so both fall
// out as unvoiced without a special case for either.
//
// This matters downstream because constitution rules say unvoiced audio
// passes through uncorrected: breaths and consonants must not be pitched.
TEST_CASE("YIN reports unvoiced for digital silence") {
    YinDetector detector;
    detector.prepare(kSampleRate, kBlockSize);

    const std::vector<float> signal = opentune::test::silence(kTestSamples);
    const PitchEstimate estimate = feedBlocks(detector, signal);

    CHECK_FALSE(estimate.voiced);
    // PitchEstimate's contract: 0 Hz is not a guess at the pitch, it is
    // the absence of one.
    CHECK(estimate.frequencyHz == 0.0f);
    CHECK(estimate.confidence == doctest::Approx(0.0));
}

TEST_CASE("YIN reports unvoiced for white noise") {
    // Several seeds, because a single unlucky draw proves nothing either
    // way. White noise is the AC6 case that a pure loudness gate cannot
    // catch: it is as loud as a sung note and has no pitch whatsoever.
    for (unsigned seed = 1; seed <= 5; ++seed) {
        YinDetector detector;
        detector.prepare(kSampleRate, kBlockSize);

        const std::vector<float> signal = opentune::test::whiteNoise(kTestSamples, seed);
        const PitchEstimate estimate = feedBlocks(detector, signal);

        INFO("seed ", seed, " -> ", estimate.frequencyHz, " Hz, confidence ", estimate.confidence);
        CHECK_FALSE(estimate.voiced);
        CHECK(estimate.frequencyHz == 0.0f);
    }
}

TEST_CASE("YIN reports voiced with high confidence for a clean sine") {
    for (const double hz : kSweepFrequenciesHz) {
        YinDetector detector;
        detector.prepare(kSampleRate, kBlockSize);

        const std::vector<float> signal =
            opentune::test::sine(static_cast<float>(hz), kSampleRate, kTestSamples);
        const PitchEstimate estimate = feedBlocks(detector, signal);

        INFO("frequency ", hz, " Hz, confidence ", estimate.confidence);
        CHECK(estimate.voiced);
        // A clean sine is about as periodic as a signal gets, so the
        // aperiodicity at the chosen lag should be near zero.
        CHECK(estimate.confidence > 0.9f);
    }
}

TEST_CASE("YIN reports unvoiced for a quiet sine only when it stops being periodic") {
    // Confidence must not be a disguised loudness meter. A sine at 1/1000
    // of full scale is still perfectly periodic, and the normalisation in
    // step 2 divides loudness out of d' entirely, so it must still read as
    // voiced at the right pitch. (Whether such a signal is worth
    // correcting is a question for the engine, not the detector.)
    YinDetector detector;
    detector.prepare(kSampleRate, kBlockSize);

    const std::vector<float> signal =
        opentune::test::sine(220.0f, kSampleRate, kTestSamples, 0.001f);
    const PitchEstimate estimate = feedBlocks(detector, signal);

    INFO("quiet sine -> ", estimate.frequencyHz, " Hz, confidence ", estimate.confidence);
    CHECK(estimate.voiced);
    CHECK(std::abs(centsError(static_cast<double>(estimate.frequencyHz), 220.0)) < 5.0);
}

TEST_CASE("YIN reports unvoiced before its analysis window has filled") {
    // The window is ~2218 samples; one block is 256. Until enough real
    // audio has accumulated, the buffer is mostly the zeros it was
    // initialised with, and analysing that would measure the padding.
    // PitchDetector's contract says return immediately and unvoiced rather
    // than block or guess.
    YinDetector detector;
    detector.prepare(kSampleRate, kBlockSize);

    const std::vector<float> signal = opentune::test::sine(440.0f, kSampleRate, kBlockSize);
    const PitchEstimate estimate = detector.process(signal.data(), kBlockSize);

    CHECK_FALSE(estimate.voiced);
    CHECK(estimate.frequencyHz == 0.0f);
}

TEST_CASE("YIN reset returns the detector to its unfilled state") {
    YinDetector detector;
    detector.prepare(kSampleRate, kBlockSize);

    const std::vector<float> signal = opentune::test::sine(440.0f, kSampleRate, kTestSamples);
    const PitchEstimate before = feedBlocks(detector, signal);
    REQUIRE(before.voiced);

    detector.reset();

    // One block after a reset is not a window, so the detector must be
    // back to reporting unvoiced rather than holding the stale estimate.
    const PitchEstimate afterReset = detector.process(signal.data(), kBlockSize);
    CHECK_FALSE(afterReset.voiced);

    // ...and it recovers the same answer once refilled.
    const PitchEstimate refilled = feedBlocks(detector, signal);
    CHECK(refilled.voiced);
    CHECK(refilled.frequencyHz == doctest::Approx(before.frequencyHz));
}

TEST_CASE("YIN gives the same answer whatever block size the audio arrives in") {
    // Block-boundary test (engine/CLAUDE.md): the detector must not be
    // sensitive to how the host happens to chop up the stream. Same audio,
    // different block sizes, same estimate.
    const std::vector<float> signal = opentune::test::sine(311.13f, kSampleRate, kTestSamples);

    const int blockSizes[] = {1, 32, 64, 128, 256, 512, 1024};
    float reference = 0.0f;
    for (const int blockSize : blockSizes) {
        YinDetector detector;
        detector.prepare(kSampleRate, 1024);

        PitchEstimate estimate{0.0f, 0.0f, false};
        int offset = 0;
        const int total = static_cast<int>(signal.size());
        while (offset < total) {
            const int n = std::min(blockSize, total - offset);
            estimate = detector.process(signal.data() + static_cast<std::size_t>(offset), n);
            offset += n;
        }

        INFO("block size ", blockSize, " -> ", estimate.frequencyHz, " Hz");
        REQUIRE(estimate.voiced);
        if (reference == 0.0f) {
            reference = estimate.frequencyHz;
        } else {
            CHECK(estimate.frequencyHz == doctest::Approx(reference));
        }
    }
    CHECK(std::abs(centsError(static_cast<double>(reference), 311.13)) < 5.0);
}
