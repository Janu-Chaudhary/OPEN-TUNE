#pragma once

#include "opentune/PitchDetector.h"

#include <vector>

namespace opentune {

// The Stage 1 pitch detector: YIN.
//
// Reference: Alain de Cheveigne and Hideki Kawahara, "YIN, a fundamental
// frequency estimator for speech and music", Journal of the Acoustical
// Society of America 111(4), April 2002, pp. 1917-1930.
// https://doi.org/10.1121/1.1458024
//
// WHY NOT PLAIN AUTOCORRELATION
//
// AutocorrelationDetector (Stage 0, still present as the baseline) asks
// "at which lag is the signal most SIMILAR to itself?". That question is
// fundamentally ambiguous: a signal that repeats every P samples also
// repeats every 2P, 3P, ... samples, and it is *exactly as similar* to
// itself at those lags. So the correlation peak at P is matched by equally
// tall peaks at 2P and 3P, and any rule for picking one of them is a
// tie-break, not a measurement. Choosing the tallest peak tends to land on
// a multiple of the period -- an octave (or more) too low. Choosing the
// first strong peak, as AutocorrelationDetector does, inverts the bias:
// when a voice's fundamental is weak and its first harmonic is strong, the
// harmonic's peak comes first and the answer is an octave too HIGH (see
// docs/decisions/0003-first-peak-autocorrelation.md). Either way, octave
// errors are structural, not a tuning problem.
//
// YIN attacks the ambiguity itself, in two moves.
//
//   1. Ask about DIFFERENCE instead of similarity:
//
//          d(tau) = sum over j in [0, W) of (x[j] - x[j + tau])^2
//
//      This is minimised, rather than maximised, at the period. On its own
//      that is no better -- d dips just as deeply at 2P as at P, and it
//      also dips at tau = 0, where it is exactly zero and useless.
//
//   2. Divide each d(tau) by the RUNNING MEAN of all differences up to
//      that lag ("cumulative mean normalised difference", CMND):
//
//          d'(0) = 1,   d'(tau) = d(tau) / [ (1/tau) * sum_{j=1..tau} d(j) ]
//
//      A value of d(tau) is no longer judged in absolute terms but
//      against how large differences have *typically* been at every
//      shorter lag. At very short lags a smooth signal barely differs from
//      itself, so d(tau) is tiny -- but so is the running mean, and the
//      ratio sits at about 1. Short lags are therefore *cheap in absolute
//      terms but expensive in normalised terms*: they can no longer win by
//      default. In fact, wherever d is still rising, d(tau) is above the
//      mean of everything before it, so d' >= 1 there by construction --
//      the entire rising slope out of tau = 0 is disqualified for free,
//      and with it every lag at a fraction of the true period. That is the
//      "detected an octave (or more) too HIGH" family of errors gone, and
//      it is the bulk of what step 2 buys (the paper measures the gross
//      error rate falling from 10.0% to 1.95% at this step).
//
//      Be precise about what step 2 does NOT do, because the usual
//      shorthand overstates it. It does not by itself separate a period
//      from its double. For a stationary periodic signal the running mean
//      has already plateaued after one period -- averaging in another
//      identical cycle does not move it -- so d(P) and d(2P) are divided
//      by the same denominator and both come out at essentially zero. This
//      repo's tests assert that tie rather than papering over it.
//
//      What step 2 does buy for the period-doubling problem is the thing
//      step 3 needs: it puts every lag, and every signal, on one common
//      scale, where roughly 1 means "no periodicity here" and 0 means
//      "perfect". Only then is a single fixed absolute threshold
//      meaningful, and only then is "take the FIRST lag under it" a
//      well-defined, signal-independent rule.
//
//   3. Threshold, first crossing (T1.5, revised by D10). Take the first tau
//      whose d' dips below a threshold, then descend to the bottom of that
//      dip -- rather than taking the global minimum, which would be a coin
//      flip between P, 2P and 3P. Because "first" now means "shortest
//      period", period doubling loses. (The paper measures the gross error
//      rate falling from 1.69% to 0.78% at this step.)
//
//      The paper's threshold is the fixed kAbsoluteThreshold. This
//      implementation uses max(kAbsoluteThreshold, kRelativeThreshold *
//      min d') instead -- identical on any signal whose true period dips
//      clearly below 0.1, and different only on breathy phonation, where
//      aspiration noise lifts the whole difference function so far that a
//      fixed bar is either crossed first at the OCTAVE or never crossed at
//      all. See the step 3 comment in YinDetector.cpp for the mechanism and
//      the measurement; it is the whole of D10.
//
//      This is NOT the same tie-break as AutocorrelationDetector's
//      first-peak rule, even though both say "first". There, the threshold
//      was a plain correlation strength, which a harmonic of the true
//      pitch passes just as easily as the fundamental -- so a strong
//      harmonic wins and the answer comes out an octave too high. Here,
//      any lag shorter than the true period lies on d's rising slope,
//      where d' >= 1, so it cannot clear the threshold at all. The two
//      steps are complementary: step 2 forecloses the too-high errors,
//      step 3 forecloses the too-low ones.
//
// Two refinements complete the algorithm:
//
//   4. Parabolic interpolation (T1.6) around the chosen lag. Integer lags
//      quantise the answer badly at high pitch -- at 1100 Hz one period is
//      only ~44 samples, so being one sample out is ~39 cents. Fitting a
//      parabola through the chosen minimum and its two neighbours recovers
//      a fractional lag and takes the error from tens of cents to a few.
//
//   5. A voiced/unvoiced decision (T1.7) read straight off d' at the
//      chosen lag. That value IS the aperiodicity: 0 means the signal
//      repeats perfectly at that lag, 1 means it is no more self-similar
//      there than at a typical lag. Silence and white noise never produce
//      a low value, so they are reported unvoiced without needing a
//      separate loudness rule.
//
// YIN's step 6 ("best local estimate", a search across neighbouring
// frames) is deliberately NOT implemented: it needs future frames, and
// constitution III forbids lookahead.
//
// See PitchDetector.h for the interface contract and the general reason a
// detector needs a longer analysis window than the block size it is fed.
class YinDetector final : public PitchDetector {
public:
    // One-time setup: sizes and allocates the ring buffer and the two
    // lag-indexed scratch arrays for the given sample rate. May allocate
    // (constitution II); never called from the audio thread.
    void prepare(double sampleRate, int maxBlockSize) override;

    // Clears buffered history back to the post-prepare() state without
    // freeing or reallocating. Real-time safe (noexcept).
    void reset() noexcept override;

    // Appends `n` samples to the ring buffer and, once a full analysis
    // window has accumulated, runs the YIN pipeline over it. Before the
    // window has filled, returns {0, 0, false} rather than analysing a
    // part-zero window. Real-time safe (noexcept): never allocates,
    // throws, locks, logs, or blocks.
    PitchEstimate process(const float* block, int n) noexcept override;

    // --- Inspection accessors -------------------------------------------
    //
    // These expose the intermediate stages of the algorithm so tests can
    // check each step on its own rather than only through the final
    // frequency. They return references to arrays sized in prepare() and
    // are const and noexcept -- reading them costs nothing and allocates
    // nothing. They are not part of the PitchDetector interface; nothing
    // in the audio path calls them.

    // The raw difference function d(tau), indexed by lag tau in
    // [0, maxLagSamples() + 1]. Valid after a process() call that had a
    // full window; all zeros before that.
    const std::vector<double>& differenceFunction() const noexcept { return m_difference; }

    // The cumulative mean normalised difference d'(tau), same indexing.
    const std::vector<double>& cumulativeMeanNormalizedDifference() const noexcept {
        return m_cmnd;
    }

    // Shortest and longest lag, in samples, that may be returned as an
    // answer -- the sample-domain image of the 65-1100 Hz detectable range
    // (specs.md section 6).
    int minLagSamples() const noexcept { return m_minLagSamples; }
    int maxLagSamples() const noexcept { return m_maxLagSamples; }

    // Length W of the comparison window each d(tau) sums over.
    int analysisWindowSamples() const noexcept { return m_analysisWindowSamples; }

private:
    // Detectable pitch range (specs.md section 6). Lags outside the range
    // these imply are never candidate answers.
    static constexpr float kMinFrequencyHz = 65.0f;
    static constexpr float kMaxFrequencyHz = 1100.0f;

    // How many multiples of the longest lag the comparison window spans.
    // 2 means the lowest pitch in range (65 Hz, the longest period) still
    // gets two full periods inside every comparison, so a dip in d(tau)
    // has to be confirmed by a second cycle rather than being a fluke of
    // one noisy one. At 48 kHz this is 2 * 739 = 1478 samples (~31 ms),
    // the same order as the "~2048-sample trailing window" specs.md
    // section 6 budgets for analysis lag.
    static constexpr int kAnalysisWindowLagMultiple = 2;

    // YIN's absolute threshold (paper, step 3). A lag qualifies as a
    // period candidate when d' dips below this. 0.1 is the paper's own
    // value and it is not arbitrary: on the normalised scale d' produces,
    // ~1 means "no periodicity at this lag" and 0 means "repeats
    // perfectly", so 0.1 says "at least 90% of the typical difference is
    // gone here". Because step 2 removed the dependence on loudness and on
    // lag, one fixed number works across signals -- which is the whole
    // reason a first-crossing rule is possible.
    static constexpr float kAbsoluteThreshold = 0.1f;

    // How much worse than the BEST dip in the frame a shorter dip may be
    // and still win (D10). Only ever used to raise step 3's threshold above
    // kAbsoluteThreshold, never to lower it:
    //
    //     threshold = max(kAbsoluteThreshold, kRelativeThreshold * min d')
    //
    // Why a relative threshold is needed at all is explained at step 3 in
    // YinDetector.cpp: broadband aspiration noise lifts the whole
    // difference function off the floor, so on a breathy voice the true
    // period's dip can sit above 0.1 while the octave's dip sits below it,
    // and the fixed threshold then fires in the wrong place.
    //
    // Why 2. For a periodic signal plus uncorrelated noise, d(P) and d(2P)
    // both bottom out on the same noise floor ~2*W*sigma^2 and d' at the
    // two is tied in principle -- which of them comes out lower is decided
    // by estimation noise, not by the signal. So the tolerance only has to
    // be wide enough to cover that scatter, and a factor of 2 (3 dB on the
    // normalised difference) is comfortably wider than the ~1.4 measured
    // between d'(P) and d'(2P) on the failing frames. It is also far
    // narrower than the gap to a genuinely wrong lag, where d' is order 1 --
    // ten times the values in play.
    //
    // Measured on testdata/synthetic, 33 cases with exact labels: the
    // octave-error rate over the two breathy cases falls to 0.00% anywhere
    // in 2.0-4.0 and every non-breathy case is bit-for-bit unchanged, so 2
    // sits inside a plateau rather than on a fitted point. Below it the
    // error returns (0.11% at 1.75, 0.64% at 1.5); at 6 and above the
    // mirror hazard appears -- the threshold climbs high enough to accept
    // noise wiggles at short lags, which step 5 then reports unvoiced, so
    // recall falls. 2 is the low, conservative end of the safe range: the
    // smallest departure from the paper's rule that clears AC7 outright.
    static constexpr float kRelativeThreshold = 2.0f;

    // Maximum aperiodicity -- d' at the chosen lag -- for a frame to count
    // as voiced (T1.7, AC6). Deliberately looser than kAbsoluteThreshold:
    // a frame whose best dip just missed the 0.1 search threshold is still
    // plainly periodic and should be corrected, so requiring 0.1 here as
    // well would throw away good frames at note onsets and on breathy
    // voices. 0.2 keeps that headroom while staying far from where
    // unpitched input actually sits -- measured on this repo's test
    // signals, white noise lands at 0.91-0.93 and digital silence at
    // exactly 1.0, so the gap either side of 0.2 is wide.
    //
    // Provisional: AC6 is "< 5% of frames misclassified" measured against
    // hand-labelled real vocals, which is T1.8's job. This value is chosen
    // from synthetic signals and should be revisited there, not treated as
    // settled.
    static constexpr float kVoicedAperiodicityMax = 0.2f;

    // Sample rate prepared with, in Hz; used to turn a lag back into a
    // frequency (f = sampleRate / lag).
    double m_sampleRate = 0.0;

    // Ring buffer of the most recent samples. Sized once in prepare() and
    // never resized (constitution II). Logically circular but implemented
    // as a flat window shifted left and refilled each call, so the
    // analysis can treat it as one contiguous, chronologically-ordered
    // block -- the same approach AutocorrelationDetector uses, and still
    // real-time safe because every copy is bounded by the fixed buffer
    // size.
    std::vector<float> m_buffer;

    // Lag-indexed scratch, both sized [0, m_topLagSamples] in prepare()
    // and fully overwritten each process() call. m_topLagSamples is one
    // past m_maxLagSamples so that a winning lag of exactly
    // m_maxLagSamples still has a right-hand neighbour for parabolic
    // interpolation (T1.6).
    std::vector<double> m_difference;
    std::vector<double> m_cmnd;

    int m_minLagSamples = 0;
    int m_maxLagSamples = 0;
    int m_topLagSamples = 0;
    int m_analysisWindowSamples = 0;

    // Samples written since the last prepare()/reset(), capped at the
    // buffer size. While below it, the window has not filled once and
    // process() reports unvoiced instead of analysing zeros.
    int m_samplesFilled = 0;
};

} // namespace opentune
