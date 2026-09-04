// T1.2 — Octave-error test set: signals with weak or missing fundamentals.
//
// The audio concept and the signals themselves live in tests/support/OctaveSignals.h
// (read that header's top comment first). This file USES that set against the current
// detector, AutocorrelationDetector, to empirically check
// docs/decisions/0003-first-peak-autocorrelation.md's prediction: because that detector
// selects the *first* qualifying correlation peak (shortest lag) rather than the global
// maximum, a weak-or-missing fundamental should make it lock onto H2 (or another early
// harmonic) and report the pitch an octave too HIGH, not too low.
//
// **The prediction did not hold for the five required cases.** Measured (see each
// TEST_CASE below): a harmonic stack with H1 entirely absent, attenuated -20 dB, or
// outweighed by H2, and a telephone-band-limited stack missing f0 and its first several
// harmonics, are ALL detected correctly -- within the same +/-20 cents this detector
// achieves on an ordinary, strong-fundamental control tone. No octave error appears.
//
// The reason is a property of autocorrelation itself, not a quirk of this detector: for
// ANY signal built from integer harmonics of f0, shifting it by exactly one period
// (1 / f0) reproduces the identical waveform, however that waveform's energy happens to
// be distributed across harmonics -- so the correlation at that lag is provably 1.0 (a
// perfect self-match), tied with lag 0 itself. A shorter candidate lag, like H2's own
// period (half of f0's), only threatens to *tie* that perfect score if the signal is
// ALSO exactly periodic at the shorter lag -- which requires every harmonic present to
// be a multiple of 2 relative to f0. Removing, attenuating, or being outweighed by H1
// does not achieve that: odd harmonics above H1 (H3, H5, H7, ...) are still present in
// every required case, and they land out of phase at H2's half-period lag (their
// contribution there is *negative* -- see the worked arithmetic in the missing-
// fundamental TEST_CASE below), pulling that shorter lag's correlation measurably below
// the 0.6 qualifying threshold before the true, longer period is ever reached.
//
// So this file adds one case beyond the five required, `evenHarmonicsOnlyStack` (only
// H2, H4, H6, H8 present -- every odd harmonic including H1 is exactly zero), which
// satisfies the condition above: the signal is then genuinely, exactly as periodic at
// 2*f0 as it is at f0, the tie decision 0003 describes is real, and the octave-high bias
// is observed exactly as predicted (for f0 low enough that 2*f0 still falls inside the
// detector's own declared 65-1100 Hz search range -- see that TEST_CASE for why high
// test frequencies are excluded).
//
// These tests PIN the current detector's ACTUAL behaviour -- both the parts that work
// and the one that fails -- rather than asserting what was expected going in. That is
// the honest outcome of this task's brief: "if the prediction is wrong, say so." It is
// also, in its own way, a second data point for decision 0003: the octave-high bias it
// documents is real, but only under a stricter condition (a signal that is genuinely,
// exactly periodic at the shorter lag) than "the fundamental is weak" alone. T1.3-T1.8
// replace this detector with YinDetector specifically to make AC7 (<1% octave errors)
// hold on real, less-idealised vocal recordings; the pinned numbers here -- both the
// passes and evenHarmonicsOnlyStack's failure -- are the "before" side of that
// comparison, not a claim that this detector already meets AC7.
#include "doctest.h"
#include "opentune/AutocorrelationDetector.h"
#include "support/OctaveSignals.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace {

using opentune::AutocorrelationDetector;
using opentune::PitchEstimate;

constexpr double kSampleRate = 48000.0;
constexpr int kBlockSize = 256;

// Long enough that the detector's internal ring buffer (roughly 2200 samples at this
// sample rate and detectable range -- see AutocorrelationDetector.cpp's prepare()) fills
// several times over, so the final estimate reflects a fully-settled analysis window
// rather than a startup transient.
constexpr int kSignalDurationSamples = 16384;

// Feeds `signal` through `detector` in kBlockSize chunks (mirroring the real block
// contract -- see engine/CLAUDE.md "Block contract": callers always deliver
// n <= maxBlockSize at a time, never the whole signal in one call) and returns the LAST
// estimate produced. The last block's estimate is the one measuring the fully-filled
// analysis window; earlier ones are still ramping up as the ring buffer fills (see
// AutocorrelationDetector::process(), which returns voiced=false until then).
PitchEstimate runDetector(AutocorrelationDetector& detector, const std::vector<float>& signal) {
    PitchEstimate last{0.0f, 0.0f, false};
    const int total = static_cast<int>(signal.size());
    int offset = 0;
    while (offset < total) {
        const int n = std::min(kBlockSize, total - offset);
        last = detector.process(signal.data() + offset, n);
        offset += n;
    }
    return last;
}

// Ratio of measured to true frequency, in octaves (log base 2): 0.0 means an exact
// match, +1.0 means exactly double (an octave too high), -1.0 means exactly half (an
// octave too low). Used to classify each measured estimate below without repeating the
// same log2 expression at every call site.
double octavesOff(float measuredHz, float trueHz) {
    return std::log2(static_cast<double>(measuredHz) / static_cast<double>(trueHz));
}

// A generous band around "exactly right": +/-20 cents is this detector's own declared
// accuracy bar (T0.4's "Done when", specs.md AC1 note -- YIN's +/-5 cents is Stage 1's
// job, not this naive detector's). 20 cents is 20/1200 = 1/60 of an octave.
constexpr double kCorrectBandOctaves = 20.0 / 1200.0;

// A generous band around "exactly one octave off": real signals here are built from
// discrete harmonics at integer-lag-quantized frequencies (this detector has no
// sub-sample interpolation -- see AutocorrelationDetector.h), so a genuine octave lock
// can land a little off from a mathematically perfect 2x. 3% of an octave (36 cents) is
// comfortably wider than the correct-band above while still nowhere near 0.5 octaves off
// (an octave error the other direction) or 0 (correct).
constexpr double kOctaveBandOctaves = 0.03;

bool isCorrect(double octaves) {
    return std::abs(octaves) <= kCorrectBandOctaves;
}

bool isOctaveHigh(double octaves) {
    return std::abs(octaves - 1.0) <= kOctaveBandOctaves;
}

// This detector's own declared maximum detectable frequency (specs.md section 6;
// AutocorrelationDetector.h's kMaxFrequencyHz). A candidate lag outside [65, 1100] Hz is
// never eligible to be returned as an answer at all -- see AutocorrelationDetector.cpp's
// lag-selection comment -- so once 2*f0 exceeds this ceiling, the octave-high alias
// cannot be selected regardless of how the underlying signal is built. Duplicated here
// (rather than #include-ing the engine header's private constant, which is not
// accessible outside the class) because it is what makes evenHarmonicsOnlyStack's
// per-frequency expectation below principled rather than an unexplained magic number.
constexpr float kDetectorMaxFrequencyHz = 1100.0f;

} // namespace

// --- Control: a normal, strong-fundamental stack must be detected correctly ----------
//
// This is the load-bearing negative control for every other test in this file: if this
// one failed, "the detector got a weak-fundamental case wrong" would be indistinguishable
// from "the detector is broken on ordinary input" or "the harmonic-stack generator itself
// is wrong". It must pass for every vocal-range frequency the octave-error cases below
// also use.
TEST_CASE("AutocorrelationDetector: control stack (strong fundamental) is detected correctly "
          "across the vocal range") {
    for (const float f0 : opentune::test::vocalRangeTestFrequenciesHz()) {
        CAPTURE(f0);
        AutocorrelationDetector detector;
        detector.prepare(kSampleRate, kBlockSize);

        const std::vector<float> signal =
            opentune::test::controlStack(f0, kSampleRate, kSignalDurationSamples);
        const PitchEstimate estimate = runDetector(detector, signal);

        REQUIRE(estimate.voiced);
        const double octaves = octavesOff(estimate.frequencyHz, f0);
        CAPTURE(estimate.frequencyHz);
        CAPTURE(octaves);
        CHECK(isCorrect(octaves));
    }
}

// --- Missing fundamental: prediction checked, and NOT confirmed -----------------------
//
// docs/decisions/0003 predicts this detector locks onto the first qualifying peak, which
// on a stack with H1 entirely absent might be expected to be H2's own period (half the
// true period) -- an octave-too-high report. MEASURED RESULT: it is not. Every tested
// frequency is detected correctly, within this detector's own +/-20 cent bar.
//
// Worked arithmetic for why, at f0 = 440 Hz (kA4Hz), using this stack's amplitudes
// (1/h for h = 2..8, h = 1 zero): the correlation sum at a lag L, for a signal that is a
// sum of harmonics a_h*sin(2*pi*h*f0*t), is proportional to
// sum_h (a_h^2 / 2) * cos(2*pi*h*f0*L). At L = 1/f0 (the true period), every harmonic's
// phase term is cos(2*pi*h) = 1, so this sum equals sum_h a_h^2/2 exactly -- the same
// value as at lag 0 -- giving a normalised correlation of exactly 1.0 by construction,
// for ANY set of harmonics. At L = 1/(2*f0) (H2's own, half, period), each harmonic's
// phase term is cos(pi*h) = +1 for even h, -1 for odd h: with H1 = 0, H2 = 1/2,
// H3 = 1/3, ..., H8 = 1/8, that sum is
// +(1/2)^2/2 - (1/3)^2/2 + (1/4)^2/2 - (1/5)^2/2 + (1/6)^2/2 - (1/7)^2/2 + (1/8)^2/2
// ~= 0.092, against ~0.264 at the true period -- a normalised ratio of ~0.35, well below
// the 0.6 qualifying threshold. The odd harmonics still present (H3, H5, H7) subtract at
// this lag; that is what keeps the half-period candidate from ever qualifying as a peak,
// so the search proceeds past it to the true (and only qualifying) period.
TEST_CASE("AutocorrelationDetector: missing-fundamental stack — measured NOT octave-high, "
          "across the vocal range") {
    for (const float f0 : opentune::test::vocalRangeTestFrequenciesHz()) {
        CAPTURE(f0);
        AutocorrelationDetector detector;
        detector.prepare(kSampleRate, kBlockSize);

        const std::vector<float> signal =
            opentune::test::missingFundamentalStack(f0, kSampleRate, kSignalDurationSamples);
        const PitchEstimate estimate = runDetector(detector, signal);

        REQUIRE(estimate.voiced);
        const double octaves = octavesOff(estimate.frequencyHz, f0);
        CAPTURE(estimate.frequencyHz);
        CAPTURE(octaves);
        // Pinned as CORRECT, not octave-high: decision 0003's predicted failure mode
        // does not reproduce here. See this file's header comment and
        // evenHarmonicsOnlyStack below for the condition under which it does.
        CHECK(isCorrect(octaves));
    }
}

// --- Attenuated fundamental (-20 dB relative to H2): same measured outcome -----------
TEST_CASE("AutocorrelationDetector: attenuated-fundamental stack — measured NOT octave-high, "
          "across the vocal range") {
    for (const float f0 : opentune::test::vocalRangeTestFrequenciesHz()) {
        CAPTURE(f0);
        AutocorrelationDetector detector;
        detector.prepare(kSampleRate, kBlockSize);

        const std::vector<float> signal =
            opentune::test::attenuatedFundamentalStack(f0, kSampleRate, kSignalDurationSamples);
        const PitchEstimate estimate = runDetector(detector, signal);

        REQUIRE(estimate.voiced);
        const double octaves = octavesOff(estimate.frequencyHz, f0);
        CAPTURE(estimate.frequencyHz);
        CAPTURE(octaves);
        // A -20 dB (not absent) fundamental still contributes a negative term at H2's
        // half-period lag (same mechanism as the missing-fundamental case above, just
        // with a small positive H1 term added back in), so this is if anything an even
        // easier case for the true period to win outright. Measured: correct at every
        // tested frequency.
        CHECK(isCorrect(octaves));
    }
}

// --- H2-dominant: same measured outcome ------------------------------------------------
TEST_CASE("AutocorrelationDetector: H2-dominant stack — measured NOT octave-high, across "
          "the vocal range") {
    for (const float f0 : opentune::test::vocalRangeTestFrequenciesHz()) {
        CAPTURE(f0);
        AutocorrelationDetector detector;
        detector.prepare(kSampleRate, kBlockSize);

        const std::vector<float> signal =
            opentune::test::h2DominantStack(f0, kSampleRate, kSignalDurationSamples);
        const PitchEstimate estimate = runDetector(detector, signal);

        REQUIRE(estimate.voiced);
        const double octaves = octavesOff(estimate.frequencyHz, f0);
        CAPTURE(estimate.frequencyHz);
        CAPTURE(octaves);
        // H1 = 0.5, H2 = 1.0 (H2 louder, but H1 far from zero) still normalises to
        // ~0.45 at the half-period lag by the same arithmetic as above -- short of the
        // 0.6 gate. Measured: correct at every tested frequency.
        CHECK(isCorrect(octaves));
    }
}

// --- Band-limited (telephone-band), f0 below the passband: same measured outcome -----
//
// Only exercised for frequencies whose fundamental itself falls below the telephone
// band (see OctaveSignals.h generateOctaveErrorCases for why) -- the
// vocalRangeTestFrequenciesHz values below kTelephoneBandLowHz (300 Hz): kLowMaleHz (90),
// kMaleHz (130), kLowFemaleHz (220). For these, H1, H2, and (for the lowest two) H3 are
// all exactly zero -- a strictly harder case than missingFundamentalStack, since more
// than one leading harmonic is gone. Measured: still detected correctly. The true
// period's correlation is still exactly 1.0 by the same argument as above regardless of
// *which* harmonics carry the signal's energy, and the surviving in-band harmonics
// (H4 upward) still include enough odd multiples of f0 to keep every shorter candidate
// lag's normalised correlation below the qualifying threshold.
TEST_CASE("AutocorrelationDetector: telephone-band stack (f0 below the passband) — measured "
          "NOT octave-high") {
    for (const float f0 : opentune::test::vocalRangeTestFrequenciesHz()) {
        if (!(f0 < opentune::test::kTelephoneBandLowHz)) {
            continue;
        }
        CAPTURE(f0);
        AutocorrelationDetector detector;
        detector.prepare(kSampleRate, kBlockSize);

        const std::vector<float> signal =
            opentune::test::telephoneBandStack(f0, kSampleRate, kSignalDurationSamples);
        const PitchEstimate estimate = runDetector(detector, signal);

        REQUIRE(estimate.voiced);
        const double octaves = octavesOff(estimate.frequencyHz, f0);
        CAPTURE(estimate.frequencyHz);
        CAPTURE(octaves);
        CHECK(isCorrect(octaves));
    }
}

// --- Even-harmonics-only: the case that DOES reproduce decision 0003's prediction ----
//
// Not one of this task's five required cases -- added after measuring that all five
// required cases above are detected correctly, to confirm decision 0003's mechanism is
// real under the right condition rather than concluding the octave-high bias does not
// exist at all. See this file's header comment for the full argument; in short: the true
// period's correlation is provably 1.0 for any harmonic signal, so a shorter candidate
// lag can only tie it (and be selected first, by this detector's shortest-lag-first
// search) if the signal is ALSO exactly periodic at that shorter lag -- which requires
// every harmonic present to be a multiple of 2 relative to f0. This stack (H2, H4, H6,
// H8 only; H1, H3, H5, H7 all exactly zero) satisfies that condition, so it is genuinely
// ambiguous between f0 and 2*f0 by construction, not just "weak" at f0.
//
// This is expected to fail ONLY while 2*f0 still falls inside the detector's own
// declared 65-1100 Hz search range (kDetectorMaxFrequencyHz): once 2*f0 exceeds that
// ceiling, the octave-high alias is not a candidate the search can select at all
// (AutocorrelationDetector.cpp restricts answers to that range), so detection reverts to
// correct -- not because the ambiguity goes away, but because one side of it is no
// longer reachable. Both outcomes are pinned below, each with the frequency-dependent
// reason spelled out, rather than picking one expectation and treating the other
// frequencies as noise.
TEST_CASE("AutocorrelationDetector: even-harmonics-only stack — measured octave-high where "
          "2*f0 is in range, confirming decision 0003") {
    for (const float f0 : opentune::test::vocalRangeTestFrequenciesHz()) {
        CAPTURE(f0);
        AutocorrelationDetector detector;
        detector.prepare(kSampleRate, kBlockSize);

        const std::vector<float> signal =
            opentune::test::evenHarmonicsOnlyStack(f0, kSampleRate, kSignalDurationSamples);
        const PitchEstimate estimate = runDetector(detector, signal);

        REQUIRE(estimate.voiced);
        const double octaves = octavesOff(estimate.frequencyHz, f0);
        CAPTURE(estimate.frequencyHz);
        CAPTURE(octaves);

        if (2.0f * f0 <= kDetectorMaxFrequencyHz) {
            // KNOWN FAILURE, confirming decision 0003: the signal is exactly as
            // periodic at 2*f0 as at f0, both lags are in range, and the shortest-
            // lag-first search picks the wrong (shorter) one. Pinned, not endorsed --
            // AC7 (<1% octave errors) is exactly what YinDetector (T1.3-T1.8) exists
            // to fix.
            CHECK(isOctaveHigh(octaves));
        } else {
            // 2*f0 exceeds the detector's own 1100 Hz search ceiling, so the
            // octave-high alias is not a reachable candidate answer at all; detection
            // reverts to correct for this reason alone, not because the underlying
            // ambiguity is resolved.
            CHECK(isCorrect(octaves));
        }
    }
}
