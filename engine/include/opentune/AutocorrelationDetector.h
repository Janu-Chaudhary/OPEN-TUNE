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
//   2. Octave bias -- and, specifically, which *direction* it errs in.
//      This implementation (see process() in the .cpp) does NOT pick the
//      single largest correlation sum in the search range. It walks lags
//      from short to long and returns the *first* interior local peak
//      whose normalised strength clears `kMinNormalizedCorrelation`. That
//      choice is deliberate: for a genuinely periodic signal, correlation
//      peaks repeat at every integer multiple of the true period, often at
//      near-identical strength (this detector's comparison window does not
//      shrink with lag -- see prepare() -- so those repeat peaks do not
//      fade out with distance the way they would against a naturally
//      decaying tone). Picking the global maximum among those near-tied
//      peaks is close to a coin flip; picking the *first* one resolves the
//      tie toward the shortest, correct period.
//
//      The cost of that choice: on a voice whose fundamental is weak or
//      missing relative to its first harmonic (common on telephone-band
//      audio, belted high notes, and many spoken male vowels where H2
//      outweighs H1), the first *harmonic* -- one octave above the true
//      pitch -- can itself form a strong, qualifying peak before the
//      (weaker) fundamental's peak is reached. In that case this detector
//      reports an octave too HIGH, not too low. This is the opposite
//      direction from naive global-max autocorrelation's textbook bias
//      (which favours long lags and tends to report an octave too low);
//      first-peak selection trades one failure mode for the other rather
//      than eliminating it.
//
//      `kMinNormalizedCorrelation` (0.6) is doing double duty here: it
//      both decides which peaks are strong enough to *qualify* as
//      candidates during the walk, and is the final voiced/unvoiced gate
//      applied to whichever lag is ultimately chosen. A harmonic peak on a
//      real, reasonably clean voice typically clears 0.6 comfortably (a
//      harmonic is still a genuine periodicity of the signal, just not the
//      fundamental one) -- which is precisely the mechanism behind the
//      octave-too-high error above: the harmonic peak is not weak, it is
//      just early.
//
//      YIN's cumulative mean normalised difference function (T1.4) is
//      designed to make this decision correctly (favour the fundamental
//      over its harmonics) rather than by lag-order alone; T1.7 (voicing)
//      is where a principled, not-double-duty voicing decision lands. This
//      detector does not attempt either; the octave-too-high bias
//      described here is a known, documented limitation of this specific
//      (first-peak) naive method, not a bug in this implementation.
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
    //
    // "Real-time safe" here means only the constitution II sense (no
    // allocation/throw/lock/log/block) -- it is NOT a claim that this call
    // fits inside a real-time audio callback's deadline. The correlation
    // search below is a full O(lags * window) scan, on the order of ~1.0
    // million multiply-adds per call at 48 kHz nominal sizing (see the
    // .cpp for the exact derivation); a 256-sample block at 48 kHz has a
    // ~5.33 ms deadline, and this detector is not expected to meet it.
    // This is a Stage 0 correctness exercise, not a deployable real-time
    // component: T1.9 benchmarks it, and Stage 1's `YinDetector` replaces
    // this costly inner loop.
    PitchEstimate process(const float* block, int n) noexcept override;

private:
    // Detectable pitch range this detector searches (specs.md section 6).
    // Lags outside [kMinFrequencyHz, kMaxFrequencyHz]'s implied lag range
    // are never treated as candidate answers: searching outside them would
    // waste the (already expensive, O(lags * window)) search and would
    // invite octave errors by allowing implausible periods to win.
    static constexpr float kMinFrequencyHz = 65.0f;
    static constexpr float kMaxFrequencyHz = 1100.0f;

    // Below this RMS level, a block is treated as silence/noise-floor and
    // reported unvoiced without running the (comparatively expensive)
    // correlation search at all. See .cpp for the chosen value and why.
    static constexpr float kMinRmsForVoiced = 0.01f;

    // Minimum normalised correlation strength (a candidate lag's
    // correlation sum divided by the zero-lag energy r(0) -- not a
    // geometric mean of the two compared segments' own energies, which is
    // why this ratio is not clamped to [0, 1] before the final clamp
    // applied to `confidence` in the .cpp) required for a lag to (a)
    // qualify as a peak during lag selection and (b) pass the final
    // voiced/unvoiced gate. See .cpp for the chosen value and why, and for
    // why using the same threshold for both jobs is part of this
    // detector's documented octave-too-high limitation (class comment
    // above).
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

    // Shortest and longest lag, in samples, that count as a candidate
    // *answer* -- the sample-domain equivalent of [kMinFrequencyHz,
    // kMaxFrequencyHz]. Computed once in prepare() from m_sampleRate by
    // widening (not rounding) toward the requested Hz range, so that range
    // is genuinely covered end to end: m_minLagSamples truncates
    // sampleRate/kMaxFrequencyHz *down* (a smaller lag than a plain round
    // would give, so its implied frequency ceiling is >= kMaxFrequencyHz,
    // never short of it) and m_maxLagSamples rounds sampleRate/
    // kMinFrequencyHz *up* (so its implied frequency floor is <=
    // kMinFrequencyHz). Only lags in [m_minLagSamples, m_maxLagSamples]
    // are ever returned as an answer -- see m_correlationBaseLag below for
    // why the correlation search itself computes a couple of lags beyond
    // this range too.
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

    // Lag corresponding to m_correlationScratch[0]. This is
    // max(1, m_minLagSamples - 1) rather than m_minLagSamples itself: lag
    // selection (see .cpp) only accepts an *interior* scratch index (one
    // with a real neighbour on both sides) as a peak, so for
    // m_minLagSamples to ever be selectable, the scratch array must hold
    // one extra lag below it to serve as that left neighbour. Symmetrically,
    // the scratch array always extends one lag past m_maxLagSamples on the
    // top end too (see prepare()). Those one or two extra entries exist
    // purely to give the true endpoints real neighbours to compare
    // against; they are never themselves eligible to be selected as the
    // answer (see .cpp) since they fall outside the declared
    // [m_minLagSamples, m_maxLagSamples] answer range.
    int m_correlationBaseLag = 0;

    // Scratch space for one lag-indexed correlation sum per candidate lag
    // in [m_correlationBaseLag, m_maxLagSamples + 1], sized once in
    // prepare() and reused (overwritten in full) every process() call --
    // never resized, so filling it is bounded, fixed-size work, not the
    // kind of growth constitution II bans. Needed because lag selection
    // (see .cpp) looks at each candidate lag's *neighbours* to find the
    // first genuine correlation peak, which means every lag's sum must be
    // computed and held before any selection decision can be made.
    std::vector<double> m_correlationScratch;
};

} // namespace opentune
