#pragma once

#include "opentune/PitchDetector.h"

#include <vector>

namespace opentune {

// The naive, textbook pitch detector: autocorrelation by direct search.
//
// The idea, in plain terms: a periodic signal looks like a (time-shifted)
// copy of itself. If you slide a copy of the signal against itself by some
// number of samples ("lag") and multiply-and-sum the overlap at every
// sample, that sum is large when the shifted copy lines back up with the
// original -- which happens exactly at multiples of the signal's period.
// So: try every plausible lag, and the lag with the largest sum is (an
// estimate of) the period. Frequency is just the reciprocal of the period
// in seconds, i.e. `sampleRate / lagInSamples`.
//
// This is the conceptual basis for YIN (Stage 1's `YinDetector`), which
// fixes this detector's two real weaknesses:
//   1. Integer-lag resolution. This detector only ever considers whole-
//      sample lags, so its frequency estimate is quantized to whatever
//      frequency gap exists between adjacent integer lags -- coarser at
//      high pitches, where one sample of lag error is a bigger fraction of
//      the (short) period. YIN adds parabolic interpolation around the
//      chosen lag (T1.6) to estimate a sub-sample period. That is why this
//      detector's accuracy bar is +/-20 cents (specs.md AC1 wants +/-5 --
//      YIN's job) and why no interpolation is added here: it is scope
//      creep for this task.
//   2. Octave bias. Plain autocorrelation sums get *larger*, on average,
//      at *longer* lags simply because low-frequency signal content (and
//      any DC/near-DC energy) correlates with itself over a wider range of
//      shifts than a genuinely periodic tone does. That can make a longer
//      lag -- half the true pitch, i.e. an octave too low -- win the
//      search even when the true-period lag is the "right" answer. YIN's
//      cumulative mean normalised difference function (T1.4) is
//      specifically designed to cancel this bias. This detector does not
//      attempt to fix it; it is a known, documented limitation of the
//      naive method, not a bug in this implementation.
//
// See PitchDetector.h for the interface contract and the general
// buffering rationale (why a detector needs a longer analysis window than
// the block size it is fed).
class AutocorrelationDetector final : public PitchDetector {
public:
    // One-time setup: sizes and allocates the internal ring buffer for the
    // given sample rate. May allocate (constitution II) -- never called
    // from the audio thread while streaming. See .cpp for how buffer size
    // is derived from the detectable pitch range (specs.md section 6:
    // 65 Hz - 1100 Hz).
    void prepare(double sampleRate, int maxBlockSize) override;

    // Clears the ring buffer contents (back to "not yet filled") without
    // freeing or reallocating anything. Real-time safe (noexcept).
    void reset() noexcept override;

    // Appends `n` samples to the ring buffer, then -- once the buffer has
    // filled at least once -- runs the autocorrelation search over the
    // most recent window and returns the resulting pitch estimate. Before
    // the buffer has filled, returns `{0.0f, 0.0f, false}` rather than
    // analysing a partial (and mostly-zero-initialised) window. Real-time
    // safe: never allocates, throws, locks, logs, or blocks (noexcept).
    PitchEstimate process(const float* block, int n) noexcept override;

private:
    // Detectable pitch range this detector searches (specs.md section 6).
    // Lags outside [kMinLagFrequencyHz, kMaxLagFrequencyHz]'s implied lag
    // range are never considered: searching them would waste the (already
    // expensive, O(lags * window)) search and would invite octave errors
    // by allowing implausible periods to win.
    static constexpr float kMinFrequencyHz = 65.0f;
    static constexpr float kMaxFrequencyHz = 1100.0f;

    // Below this RMS level, a block is treated as silence/noise-floor and
    // reported unvoiced without running the (comparatively expensive)
    // correlation search at all. See .cpp for the chosen value and why.
    static constexpr float kMinRmsForVoiced = 0.01f;

    // Minimum normalised correlation strength (best lag's correlation sum
    // divided by the zero-lag energy) required to call a frame voiced. See
    // .cpp for the chosen value and why.
    static constexpr float kMinNormalizedCorrelation = 0.6f;

    // How many times the maximum lag the ring buffer holds. The
    // correlation search needs, for the longest lag it tries, a window of
    // comparison samples *in addition to* that lag's worth of "shift" --
    // so the buffer holds analysisWindowSamples (kAnalysisWindowLagMultiple
    // * maxLagSamples) + maxLagSamples of history. See .cpp.
    static constexpr int kAnalysisWindowLagMultiple = 2;

    // Sample rate this detector was prepared with, in Hz. Stored so
    // process() can convert a winning lag (in samples) back to a
    // frequency (in Hz) without recomputing it from scratch each call.
    double m_sampleRate = 0.0;

    // Ring buffer of the most recent samples seen, sized in prepare() and
    // never resized (constitution II). Logically circular, but
    // implemented here as a flat window that is shifted left and
    // refilled each process() call -- see .cpp for why that is still
    // real-time safe.
    std::vector<float> m_buffer;

    // Shortest and longest lag, in samples, worth searching -- the
    // sample-domain equivalent of [kMinFrequencyHz, kMaxFrequencyHz].
    // Computed once in prepare() from m_sampleRate.
    int m_minLagSamples = 0;
    int m_maxLagSamples = 0;

    // Length, in samples, of the comparison window used when summing each
    // lag's correlation (m_buffer.size() - m_maxLagSamples). Computed once
    // in prepare().
    int m_analysisWindowSamples = 0;

    // How many samples have been written into m_buffer since the last
    // prepare()/reset(), capped at m_buffer.size(). While this is less
    // than m_buffer.size(), the buffer has not filled once yet and
    // process() must report unvoiced rather than analyse a partial window.
    int m_samplesFilled = 0;

    // Scratch space for one lag-indexed correlation sum per candidate lag
    // in [m_minLagSamples, m_maxLagSamples], sized once in prepare() and
    // reused (overwritten in full) every process() call -- never resized,
    // so filling it is bounded, fixed-size work, not the kind of growth
    // constitution II bans. Needed because lag selection (see .cpp) looks
    // at each candidate lag's *neighbours* to find the first genuine
    // correlation peak, which means every lag's sum must be computed and
    // held before any selection decision can be made.
    std::vector<double> m_correlationScratch;
};

} // namespace opentune
