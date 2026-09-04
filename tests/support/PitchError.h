// Pitch-detection error measurement harness, usable against ANY
// PitchDetector implementation (AutocorrelationDetector today, YinDetector
// in Stage 1). This is what turns "AC1 wants +/-5 cents, AC7 wants <1%
// octave errors" into a number you can actually compute, and it is the
// instrument every later Stage 1 task (T1.3-T1.8) is judged against -- so
// its own correctness matters as much as any engine code. See
// test_pitch_error.cpp for the hand-computed known-answer checks that
// guard against a silently-wrong harness.
//
// This is test support, not engine code (as Signals.h documents): the
// real-time rules (no allocation, no exceptions in hot paths) do not apply
// here. std::vector, std::function, and std::unique_ptr are all fine.
#pragma once

#include "opentune/PitchDetector.h"

#include "support/Signals.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <limits>
#include <memory>
#include <vector>

namespace opentune::test {

// Cents error between a measured and a reference frequency:
// 1200 * log2(measured / reference) (engine/CLAUDE.md "Numerics";
// specs.md section 6). Positive means measured is sharp of reference,
// negative means flat.
//
// NaN-safe: returns NaN -- not 0, not +/-infinity -- whenever either input
// is not a positive, finite frequency (e.g. an unvoiced PitchEstimate's
// frequencyHz == 0.0f, or a NaN/negative value from a misbehaving
// detector). 0 would silently claim perfect accuracy for a measurement
// that does not exist; +/-infinity from log2(0) or log2(negative-as-nan)
// would poison every downstream mean/max it touches without being
// obviously wrong. NaN is the one value every caller in this file already
// has to check for (std::isnan), so a missing measurement can never be
// mistaken for a correct one.
inline float cents(float measured, float reference) {
    if (!std::isfinite(measured) || !std::isfinite(reference) || measured <= 0.0f ||
        reference <= 0.0f) {
        return std::numeric_limits<float>::quiet_NaN();
    }
    return 1200.0f * static_cast<float>(
                         std::log2(static_cast<double>(measured) / static_cast<double>(reference)));
}

// One test tone's result from a sweep.
struct PitchErrorSample {
    // The reference (true) tone frequency that was fed in.
    float frequencyHz = 0.0f;

    // The representative error for this frequency: the *median* cents()
    // error over all voiced frames at or after the configured warm-up
    // period. Median (not mean) so a single fleeting bad frame cannot
    // swing an otherwise-accurate frequency's reported error. NaN if the
    // detector never reported a voiced frame after warm-up -- see
    // SweepResult::failuresAbove(), which treats that as a failure, not a
    // pass by omission.
    float errorCents = std::numeric_limits<float>::quiet_NaN();

    // Fraction of ALL frames for this tone (including the warm-up period)
    // that were reported voiced. Deliberately includes warm-up: a
    // detector that is slow to lock on should show up here, not be hidden
    // by discarding exactly the frames that would reveal it.
    double voicedFraction = 0.0;

    // Frame counts backing voicedFraction, kept alongside it so a sweep
    // over many frequencies can compute one overall voiced fraction by
    // summing counts rather than re-deriving them from a lossy average of
    // fractions.
    int totalFrames = 0;
    int voicedFrames = 0;
};

// Summary statistics over a whole sweep, plus the ability to name exactly
// which frequencies failed a threshold.
struct SweepResult {
    std::vector<PitchErrorSample> perFrequency;

    // Median / mean / max of |errorCents| across frequencies that produced
    // a real (non-NaN) error. NaN if no frequency ever reported one.
    float medianAbsErrorCents = std::numeric_limits<float>::quiet_NaN();
    float meanAbsErrorCents = std::numeric_limits<float>::quiet_NaN();
    float maxAbsErrorCents = std::numeric_limits<float>::quiet_NaN();

    // Fraction of voiced frames across the entire sweep (all frequencies,
    // all frames, including warm-up).
    double voicedFraction = 0.0;

    // Every frequency whose |errorCents| exceeds thresholdCents, OR that
    // never produced a real error at all (errorCents is NaN -- the
    // detector never locked on after warm-up). A harness that only says
    // "max error 40 cents" is much less useful than one that names the
    // frequency; this is that naming. Returned in sweep order, each
    // paired with its actual measured error, so a failing run can be
    // debugged from the report alone.
    std::vector<PitchErrorSample> failuresAbove(float thresholdCents) const {
        std::vector<PitchErrorSample> failures;
        for (const PitchErrorSample& sample : perFrequency) {
            const bool neverLockedOn = std::isnan(sample.errorCents);
            const bool tooFarOff = !neverLockedOn && std::abs(sample.errorCents) > thresholdCents;
            if (neverLockedOn || tooFarOff) {
                failures.push_back(sample);
            }
        }
        return failures;
    }
};

// Sweep parameters. Defaults are sized for AutocorrelationDetector's
// ~2200-sample analysis window at 48 kHz (see AutocorrelationDetector.cpp)
// but are generous enough for any detector with a comparable or shorter
// window -- a detector needing more warm-up than this will show it as a
// worse-than-expected voicedFraction, which is itself useful information.
struct SweepConfig {
    double sampleRate = 48000.0;

    // Fed to the detector one block at a time, each call's n <= blockSize
    // -- exactly the PitchDetector::process contract a real host follows
    // (specs.md section 6: 256 nominal, but any block size the detector
    // is prepare()'d with must work).
    int blockSize = 256;

    // Length of each test tone, in samples. 9600 at 48 kHz (0.2 s) is the
    // same figure test_autocorrelation_detector.cpp uses to let that
    // detector's ~2200-sample window fill and then hold several windows'
    // worth of steady signal.
    int toneLengthSamples = 9600;

    // Blocks discarded from the *start* of each tone before computing that
    // frequency's errorCents -- gives the detector's internal analysis
    // window time to fill with real signal rather than the
    // zero-initialised startup state, so the reported error reflects
    // steady-state accuracy. 16 blocks * 256 samples = 4096 samples,
    // comfortably past AutocorrelationDetector's ~2200-sample window,
    // while still leaving most of a 9600-sample tone (37.5 blocks total)
    // for measurement. voicedFraction is NOT affected by this cutoff --
    // it counts every frame, warm-up included.
    int warmupBlocks = 16;
};

// One per semitone from C2 (65.41 Hz) up to C6 (1046.50 Hz) -- the
// declared detectable range (specs.md section 6: 65-1100 Hz, "C2-C6").
// D-flat 6 (the next semitone, ~1108.7 Hz) would exceed the declared
// 1100 Hz ceiling, so C6 is the last point. Using exact musical semitone
// frequencies (MIDI->Hz: 440 * 2^((m-69)/12), A4 = MIDI 69 = 440 Hz --
// engine/CLAUDE.md "Numerics") rather than an arbitrary linear or log grid
// means every point in this default sweep is independently checkable by
// hand, which matters for a harness whose own correctness must be
// verifiable (see test_pitch_error.cpp).
inline std::vector<float> defaultSweepFrequenciesHz() {
    std::vector<float> frequencies;
    for (int midi = 36; midi <= 84; ++midi) {
        const double hz = 440.0 * std::pow(2.0, (static_cast<double>(midi) - 69.0) / 12.0);
        frequencies.push_back(static_cast<float>(hz));
    }
    return frequencies;
}

// Feeds one test tone at `frequencyHz` through `detector` in fixed
// `config.blockSize` blocks (the last, possibly short, block is fed as
// n < blockSize, which is a legal call under the PitchDetector contract)
// and returns that frequency's PitchErrorSample. Calls detector.reset()
// first so a caller can reuse one detector instance across frequencies
// without a stale window leaking between them (runPitchSweep below does
// not rely on this -- it uses a fresh instance per frequency -- but a
// caller measuring a single frequency directly, as the known-answer tests
// do, needs it).
inline PitchErrorSample measureOneFrequency(PitchDetector& detector, float frequencyHz,
                                            const SweepConfig& config) {
    detector.reset();
    const std::vector<float> signal =
        sine(frequencyHz, config.sampleRate, config.toneLengthSamples);

    std::vector<float> postWarmupErrors;
    int voicedFrames = 0;
    int totalFrames = 0;
    int blockIndex = 0;

    std::size_t offset = 0;
    while (offset < signal.size()) {
        const int n = static_cast<int>(std::min<std::size_t>(
            static_cast<std::size_t>(config.blockSize), signal.size() - offset));
        const PitchEstimate estimate = detector.process(signal.data() + offset, n);
        offset += static_cast<std::size_t>(n);

        ++totalFrames;
        if (estimate.voiced) {
            ++voicedFrames;
            if (blockIndex >= config.warmupBlocks) {
                const float err = cents(estimate.frequencyHz, frequencyHz);
                if (!std::isnan(err)) {
                    postWarmupErrors.push_back(err);
                }
            }
        }
        ++blockIndex;
    }

    PitchErrorSample sample;
    sample.frequencyHz = frequencyHz;
    sample.totalFrames = totalFrames;
    sample.voicedFrames = voicedFrames;
    sample.voicedFraction =
        (totalFrames > 0) ? static_cast<double>(voicedFrames) / static_cast<double>(totalFrames)
                          : 0.0;

    if (!postWarmupErrors.empty()) {
        // Median of the post-warm-up errors: robust to a single outlier
        // frame the way a mean would not be.
        std::sort(postWarmupErrors.begin(), postWarmupErrors.end());
        const std::size_t mid = postWarmupErrors.size() / 2;
        sample.errorCents = (postWarmupErrors.size() % 2 == 0)
                                ? 0.5f * (postWarmupErrors[mid - 1] + postWarmupErrors[mid])
                                : postWarmupErrors[mid];
    }
    // Else: leave errorCents as NaN -- never voiced after warm-up.

    return sample;
}

// Runs `makeDetector()` once per frequency in `frequencies`, each time
// against a *fresh* detector instance: prepare() once, then process() once
// per fixed block -- exactly the pattern a real host follows. A fresh
// instance per frequency means one frequency's settled ring-buffer state
// can never leak into the next's.
//
// Swapping in a different detector to compare is a one-line change: only
// the factory differs, e.g.
//   runPitchSweep([] { return std::make_unique<AutocorrelationDetector>(); });
//   runPitchSweep([] { return std::make_unique<YinDetector>(); });
inline SweepResult
runPitchSweep(const std::function<std::unique_ptr<PitchDetector>()>& makeDetector,
              const std::vector<float>& frequencies = defaultSweepFrequenciesHz(),
              const SweepConfig& config = SweepConfig{}) {
    SweepResult result;
    result.perFrequency.reserve(frequencies.size());

    long long totalFrames = 0;
    long long totalVoicedFrames = 0;

    for (float hz : frequencies) {
        std::unique_ptr<PitchDetector> detector = makeDetector();
        detector->prepare(config.sampleRate, config.blockSize);

        const PitchErrorSample sample = measureOneFrequency(*detector, hz, config);
        result.perFrequency.push_back(sample);

        totalFrames += sample.totalFrames;
        totalVoicedFrames += sample.voicedFrames;
    }

    result.voicedFraction = (totalFrames > 0) ? static_cast<double>(totalVoicedFrames) /
                                                    static_cast<double>(totalFrames)
                                              : 0.0;

    // Aggregate median/mean/max over frequencies that produced a real
    // (non-NaN) error. A frequency that never locked on contributes to
    // failuresAbove() but has no number to average into a median/mean/max.
    std::vector<float> absErrors;
    for (const PitchErrorSample& sample : result.perFrequency) {
        if (!std::isnan(sample.errorCents)) {
            absErrors.push_back(std::abs(sample.errorCents));
        }
    }

    if (!absErrors.empty()) {
        std::vector<float> sorted = absErrors;
        std::sort(sorted.begin(), sorted.end());
        const std::size_t mid = sorted.size() / 2;
        result.medianAbsErrorCents =
            (sorted.size() % 2 == 0) ? 0.5f * (sorted[mid - 1] + sorted[mid]) : sorted[mid];

        double sum = 0.0;
        for (float e : absErrors) {
            sum += static_cast<double>(e);
        }
        result.meanAbsErrorCents = static_cast<float>(sum / static_cast<double>(absErrors.size()));

        result.maxAbsErrorCents = *std::max_element(absErrors.begin(), absErrors.end());
    }

    return result;
}

} // namespace opentune::test
