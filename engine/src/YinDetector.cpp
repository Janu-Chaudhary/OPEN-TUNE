#include "opentune/YinDetector.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace opentune {

void YinDetector::prepare(double sampleRate, int maxBlockSize) {
    // maxBlockSize does not affect sizing: the ring buffer is shifted and
    // refilled by however many samples process() is handed, so any
    // n <= maxBlockSize works without knowing maxBlockSize up front. Kept
    // only because the PitchDetector interface requires it.
    (void)maxBlockSize;

    m_sampleRate = sampleRate;

    // Sample-domain image of the detectable range. A higher frequency is a
    // SHORTER period, so the minimum lag comes from the maximum frequency.
    // Both bounds widen outward rather than rounding, so 65-1100 Hz is
    // genuinely covered end to end: truncation gives the smallest lag
    // whose implied frequency ceiling is still >= kMaxFrequencyHz, and
    // std::ceil gives the largest lag whose implied floor is still
    // <= kMinFrequencyHz. std::max(1, ...) keeps lag 0 -- the trivial
    // "signal equals itself unshifted" case, never a pitch -- out of the
    // answer range even at absurd sample rates.
    m_minLagSamples =
        std::max(1, static_cast<int>(sampleRate / static_cast<double>(kMaxFrequencyHz)));
    m_maxLagSamples =
        static_cast<int>(std::ceil(sampleRate / static_cast<double>(kMinFrequencyHz)));

    // One lag beyond the answer range, so a winning lag of exactly
    // m_maxLagSamples still has a right neighbour to interpolate against.
    m_topLagSamples = m_maxLagSamples + 1;

    m_analysisWindowSamples = kAnalysisWindowLagMultiple * m_maxLagSamples;

    // d(tau) for the widest lag compares buffer[j] with buffer[j + tau]
    // for j across the whole comparison window, so the buffer must hold
    // W + topLag samples for every lag's comparison to stay in bounds.
    const int bufferSize = m_analysisWindowSamples + m_topLagSamples;

    // Everything below is allocated exactly once, here, and thereafter
    // only overwritten in place (constitution II, engine/CLAUDE.md).
    m_buffer.assign(static_cast<std::size_t>(bufferSize), 0.0f);
    m_difference.assign(static_cast<std::size_t>(m_topLagSamples + 1), 0.0);
    m_cmnd.assign(static_cast<std::size_t>(m_topLagSamples + 1), 0.0);
    m_samplesFilled = 0;
}

void YinDetector::reset() noexcept {
    // Bounded, fixed-size work over arrays that were sized in prepare()
    // and never grow -- not the input-dependent cost constitution II
    // bans. Zeroing the buffer stops a stale, partly-filled window from
    // leaking into the next analysis.
    std::fill(m_buffer.begin(), m_buffer.end(), 0.0f);
    std::fill(m_difference.begin(), m_difference.end(), 0.0);
    std::fill(m_cmnd.begin(), m_cmnd.end(), 0.0);
    m_samplesFilled = 0;
}

PitchEstimate YinDetector::process(const float* block, int n) noexcept {
    // Defensive: prepare() is always called before streaming per the
    // PitchDetector contract, but an unprepared detector must not read
    // out of bounds.
    const int bufferSize = static_cast<int>(m_buffer.size());
    if (bufferSize == 0 || n <= 0) {
        return PitchEstimate{0.0f, 0.0f, false};
    }

    // --- Append this block to the ring buffer -----------------------------
    //
    // Causal by construction (constitution III): only samples already
    // received are ever read, and the newest sample the analysis can see
    // is block[n - 1], the one currently being produced.
    if (n >= bufferSize) {
        // This block alone overflows the window: keep only its most recent
        // bufferSize samples. Discarding samples older than the window is
        // not lookahead.
        std::copy(block + (n - bufferSize), block + n, m_buffer.begin());
        m_samplesFilled = bufferSize;
    } else {
        std::copy(m_buffer.begin() + n, m_buffer.end(), m_buffer.begin());
        std::copy(block, block + n, m_buffer.end() - n);
        m_samplesFilled = std::min(bufferSize, m_samplesFilled + n);
    }

    // Window has not filled once yet: analysing it would mean measuring
    // real signal against the zeros the buffer was initialised with.
    // Report unvoiced rather than guess.
    if (m_samplesFilled < bufferSize) {
        return PitchEstimate{0.0f, 0.0f, false};
    }

    // --- Step 1 (T1.3): the difference function ---------------------------
    //
    //     d(tau) = sum over j in [0, W) of (x[j] - x[j + tau])^2
    //
    // "How different is this window from itself, shifted by tau samples?"
    // A signal that repeats every P samples is identical to itself P
    // samples later, so d(P) collapses toward zero; anywhere else the two
    // copies are out of step and the squared differences accumulate. d(0)
    // is exactly zero -- shifting by nothing changes nothing -- which is
    // why lag 0 is never a candidate answer.
    //
    // Every d(tau) sums over the SAME fixed window length W, so a dip at a
    // long lag is not penalised for having fewer samples to average over,
    // and the values stay comparable across the whole lag range. That
    // matters for step 2, which compares them directly.
    //
    // Cost: (topLag + 1) * W subtract-multiply-adds. At 48 kHz that is
    // 740 * 1478 = ~1.09 million per call -- the O(W * L) price the task
    // brief warns about, and what T1.9 will measure against the 5.33 ms
    // block budget.
    const int windowSamples = m_analysisWindowSamples;
    for (int tau = 0; tau <= m_topLagSamples; ++tau) {
        double sum = 0.0;
        for (int j = 0; j < windowSamples; ++j) {
            const double delta = static_cast<double>(m_buffer[static_cast<std::size_t>(j)]) -
                                 static_cast<double>(m_buffer[static_cast<std::size_t>(j + tau)]);
            sum += delta * delta;
        }
        m_difference[static_cast<std::size_t>(tau)] = sum;
    }

    // --- Step 2 (T1.4): cumulative mean normalised difference -------------
    //
    //     d'(0) = 1,   d'(tau) = d(tau) / [ (1/tau) * sum_{j=1..tau} d(j) ]
    //
    // Divide each difference by the running mean of every difference at a
    // shorter lag. This is the step that kills octave errors, and it does
    // it by changing what a value *means*: d'(tau) is no longer "how
    // different", it is "how different COMPARED WITH how different this
    // signal usually is at shorter lags". Consequences, in order of
    // importance:
    //
    //   * The rising slope out of lag 0 is disqualified for free. While d
    //     is still increasing, d(tau) is by definition above the mean of
    //     everything before it, so d' >= 1 there. Every lag at a fraction
    //     of the true period -- the half-period, the third-period -- sits
    //     on that slope. The small raw differences near lag 0 that a naive
    //     minimum-search would fall straight into are now the *worst*
    //     scores in the function, which is why d' needs no artificial
    //     lower bound on the search.
    //
    //   * The values become comparable across lags AND across signals
    //     (loudness cancels in the ratio): about 1 means "no periodicity
    //     here", 0 means "perfect". That is what makes the single fixed
    //     absolute threshold in step 3 possible at all.
    //
    //   * What it does NOT do, stated plainly because the usual shorthand
    //     overstates it: this step does not separate a period from its
    //     double. For a stationary periodic signal the running mean has
    //     plateaued after one period, so d(P) and d(2P) get the same
    //     denominator and both land near 0. Period doubling is killed by
    //     step 3 taking the FIRST lag under the threshold, which is only a
    //     well-defined rule because this step put all lags on one scale.
    //
    // d'(0) is defined as 1 rather than computed: d(0) is exactly 0, so
    // the ratio would be 0/0, and lag 0 is never a pitch anyway.
    //
    // Cost: one division per lag, ~740 per call -- negligible beside step 1.
    m_cmnd[0] = 1.0;
    double runningSum = 0.0;
    for (int tau = 1; tau <= m_topLagSamples; ++tau) {
        const double dTau = m_difference[static_cast<std::size_t>(tau)];
        runningSum += dTau;
        // runningSum is 0 only when every difference so far is 0, i.e.
        // digital silence (or a perfectly constant signal). Reporting 1.0
        // there is the honest answer -- "no more self-similar at this lag
        // than anywhere else" -- and it keeps silence out of the voiced
        // decision in step 5 without needing a separate loudness gate
        // (engine/CLAUDE.md: guard every division by a value that can be
        // zero).
        m_cmnd[static_cast<std::size_t>(tau)] =
            (runningSum > 0.0) ? (dTau * static_cast<double>(tau) / runningSum) : 1.0;
    }

    // --- Step 3 (T1.5, revised by D10): threshold-crossing, first dip -----
    //
    // Walk lags from short to long and take the FIRST one whose d' dips
    // below a threshold, then slide down to the bottom of that dip. Not the
    // global minimum -- that is a coin flip between P, 2P and 3P, which all
    // score alike for a stationary tone (the T1.4 test asserts exactly that
    // tie). "First" means "shortest period", so period doubling loses by
    // construction. The paper measures this step taking the gross error
    // rate from 1.69% to 0.78%.
    //
    // Note how differently this behaves from AutocorrelationDetector's
    // superficially similar "first qualifying peak" rule. There the
    // threshold was a plain correlation strength, which a harmonic of the
    // true pitch clears as easily as the fundamental -- so a strong
    // harmonic wins first and the answer is an octave high (see
    // docs/decisions/0003). Here every lag shorter than the true period
    // sits on d's rising slope where d' >= 1, so it cannot clear the
    // threshold at all. Both steps say "first"; only this one has already
    // disqualified the short lags.
    //
    // WHY THE THRESHOLD IS NOT SIMPLY 0.1 (D10)
    //
    // The paper's threshold is the fixed kAbsoluteThreshold. That is
    // correct as long as the true period's dip actually reaches it. On a
    // BREATHY voice it does not, and the reason is worth understanding
    // because it is a property of the signal, not of the code.
    //
    // Breathy phonation is a periodic glottal pulse train plus broadband
    // aspiration noise hissing through folds that never fully close. Noise
    // is uncorrelated with itself at every non-zero lag, so it adds roughly
    // the same constant floor ~2*W*sigma^2 to d(tau) EVERYWHERE. The dip at
    // the true period no longer reaches near-zero; it bottoms out on that
    // floor -- and so does the dip at 2P, at 3P, at every multiple. Once
    // the floor is high enough that d'(P) sits above 0.1, one of two things
    // happens, and measured on testdata/synthetic (docs/decisions/0008)
    // both did:
    //
    //   * d'(2P) is still under 0.1 while d'(P) is just over it (this was
    //     51% of frames at HNR 10 dB, with d'(P) ~ 0.117 against d'(2P) ~
    //     0.088). The walk sails straight past the fundamental and stops at
    //     the octave. The fixed threshold did not fail to fire -- it fired
    //     in the wrong place.
    //
    //   * nothing at all clears 0.1 (100% of frames at HNR 5 dB). The old
    //     code then fell back to the GLOBAL minimum of d' -- the one rule
    //     the paragraph above calls a coin flip -- and lost the toss toward
    //     2P about a third of the time.
    //
    // Both are the same defect: an absolute threshold stops meaning
    // anything once the whole function has been lifted above it. And the
    // wrong answer shipped as CONFIDENT, because d'(2P) ~ 0.09 is far under
    // step 5's 0.2 voicing gate -- so tightening that gate cannot fix this,
    // and measurement confirmed it cannot.
    //
    // The repair keeps the paper's rule and makes the threshold adapt only
    // when it has to:
    //
    //     threshold = max(kAbsoluteThreshold, kRelativeThreshold * dPrimeMin)
    //
    // where dPrimeMin is the deepest dip anywhere in the answer range. On
    // any signal where some lag genuinely reaches 0.1 with room to spare
    // this is exactly kAbsoluteThreshold and the behaviour is unchanged --
    // it can only ever raise the bar's floor, never lower it below the
    // paper's value. On a noise-lifted function it says instead: "the best
    // this frame can do is dPrimeMin; accept the SHORTEST period that comes
    // within a factor of that, because P and 2P are tied in principle and
    // the shorter one is the period."
    //
    // Two consequences worth stating:
    //
    //   * The fallback is gone, not relocated. The global minimum always
    //     satisfies this test (its d' equals dPrimeMin, and the factor is
    //     >= 1), so the search always terminates with a lag; the old
    //     "nothing qualified" branch cannot arise.
    //
    //   * The mirror hazard -- a shallow sub-period dip from noise winning
    //     and giving octave-HIGH errors -- is bounded by step 2 and by step
    //     5 together. Step 2 puts the entire rising slope out of lag 0 at
    //     d' >= 1, an order of magnitude above the dips in play, so a noise
    //     wiggle there does not qualify unless the frame is essentially
    //     unpitched; and if the threshold does climb that high, the lag it
    //     accepts carries a d' above the 0.2 voicing gate and the frame is
    //     reported unvoiced rather than wrong. Measured: the factor can be
    //     raised to 4 with no change at all to any synthetic case, and only
    //     at 6 and above does recall start to fall.
    int bestLag = m_minLagSamples;
    double dPrimeMin = m_cmnd[static_cast<std::size_t>(m_minLagSamples)];
    for (int tau = m_minLagSamples + 1; tau <= m_maxLagSamples; ++tau) {
        if (m_cmnd[static_cast<std::size_t>(tau)] < dPrimeMin) {
            dPrimeMin = m_cmnd[static_cast<std::size_t>(tau)];
        }
    }

    const double threshold = std::max(static_cast<double>(kAbsoluteThreshold),
                                      static_cast<double>(kRelativeThreshold) * dPrimeMin);

    for (int tau = m_minLagSamples; tau <= m_maxLagSamples; ++tau) {
        // The second clause is what guarantees this loop always finds a
        // lag: the global minimum satisfies it by definition, whatever the
        // threshold turned out to be. Without it a factor below 1 could
        // leave the search empty, which is the state the old fallback
        // existed to paper over.
        const double dPrime = m_cmnd[static_cast<std::size_t>(tau)];
        if (dPrime >= threshold && dPrime > dPrimeMin) {
            continue;
        }
        // Found the first qualifying dip. The threshold is crossed on the
        // way DOWN, a sample or two before the actual bottom, so walk
        // forward while d' is still falling. Taking the crossing itself
        // would bias every estimate slightly short.
        while (tau + 1 <= m_maxLagSamples &&
               m_cmnd[static_cast<std::size_t>(tau + 1)] < m_cmnd[static_cast<std::size_t>(tau)]) {
            ++tau;
        }
        bestLag = tau;
        break;
    }

    // --- Step 5 (T1.7): voiced/unvoiced from the aperiodicity -------------
    //
    // Steps 1-3 always name a lag -- step 3's threshold is relative as well
    // as absolute, so the deepest dip always qualifies and the search never
    // comes back empty, even on audio with no periodicity in it at all.
    // Something has to decide whether that lag means anything, and YIN has
    // already computed the number that answers it.
    //
    // This runs BEFORE step 3b and step 4, not after, and the ordering is
    // deliberate. It means the voicing decision is taken on step 3's lag
    // alone -- exactly the value it was taken on before T1.8 existed -- so
    // the octave repair below cannot turn an unvoiced frame voiced, cannot
    // move recall, and cannot move AC6. It can only change WHICH pitch a
    // frame that was already going to be reported voiced is given. A repair
    // that quietly changed the voiced population would be improving one
    // number by moving frames out of the denominator of another.
    //
    // This is also what bounds D10's adaptive threshold from above. On an
    // unpitched frame the threshold rises with the (high) minimum d', so it
    // may well accept some short-lag noise wiggle -- but that wiggle's own
    // d' is then above the gate here and the frame is reported unvoiced.
    // The relative threshold can cost recall on genuinely hopeless audio;
    // it cannot turn noise into a confident wrong pitch.
    //
    // d' at the chosen lag IS the aperiodicity. No extra statistic, no
    // loudness rule: 0 means the signal repeats perfectly at that lag,
    // ~1 means it is no more self-similar there than at any random lag.
    // Measured on this repo's own test signals at 48 kHz: a clean sine
    // lands below 0.01, white noise lands at 0.91-0.93 across five seeds,
    // and digital silence is exactly 1.0 by the guarded ratio in step 2.
    // That is a wide, unambiguous gap, and it closes AC6 without a
    // separate silence gate -- which matters, because a loudness gate
    // cannot catch white noise at all: noise is as loud as a sung note and
    // has no pitch whatsoever.
    if (m_cmnd[static_cast<std::size_t>(bestLag)] >= static_cast<double>(kVoicedAperiodicityMax)) {
        // PitchEstimate's contract: 0 Hz is not a guess at the pitch, it
        // is the absence of one. Downstream, unvoiced audio passes through
        // uncorrected, so breaths and consonants are never pitched.
        return PitchEstimate{0.0f, 0.0f, false};
    }

    // --- Step 3b (T1.8): confirm the lag is not half the period -----------
    //
    // Everything up to here treats "the first dip under the bar" as the
    // answer. On a voice whose ODD harmonics are all weak, that dip can be
    // at HALF the period, and no choice of bar fixes it -- see the long
    // note on kOctaveDoubleMax in YinDetector.h for the arithmetic and for
    // why six threshold variants were measured and rejected first.
    //
    // The idea in one sentence: a real period cannot be beaten by its own
    // double. Differences between one cycle and the next accumulate as the
    // lag grows -- so if L is the period, d'(2L) is at best about equal to
    // d'(L) and in practice several times larger. If instead L is HALF the
    // period, the mismatch at L is the odd-harmonic part, which inverts
    // every L and therefore cancels at every EVEN multiple of L: d'(2L)
    // and d'(4L) collapse while d'(L) and d'(3L) do not. That alternation
    // is a comb, and a comb is what is checked here -- one deep even
    // multiple could be a coincidence (on a heavily shimmering voice, two
    // cycles apart really can match better than one), two in the right
    // places is a structure.
    //
    // Cost: four array reads and three comparisons, on a frame that has
    // already survived the voicing gate. No allocation, no branching on
    // anything but already-computed values (constitution II).
    //
    // Note what is NOT done: this promotes at most once, and never walks
    // further. 4L is checked because it confirms the comb, not because 4L
    // is a candidate answer. A repair that kept doubling would be a search,
    // and a search over multiples is the "coin flip between P, 2P and 3P"
    // that step 3 exists to avoid.
    if (4 * bestLag <= m_maxLagSamples) {
        const double dPrimeAtLag = m_cmnd[static_cast<std::size_t>(bestLag)];
        const double dPrimeAtDouble = m_cmnd[static_cast<std::size_t>(2 * bestLag)];
        const double dPrimeAtQuadruple = m_cmnd[static_cast<std::size_t>(4 * bestLag)];
        if (dPrimeAtDouble < static_cast<double>(kOctaveDoubleMax) * dPrimeAtLag &&
            dPrimeAtQuadruple < static_cast<double>(kOctaveQuadrupleMax) * dPrimeAtLag) {
            bestLag *= 2;
            // Slide to the bottom of the dip at the doubled lag, the same
            // way step 3 does at the crossing. The comb argument locates
            // the period to within a sample or two of 2L; it does not
            // promise 2L is the exact minimum, and the parabola below
            // wants to be centred on the real one.
            while (bestLag + 1 <= m_maxLagSamples &&
                   m_cmnd[static_cast<std::size_t>(bestLag + 1)] <
                       m_cmnd[static_cast<std::size_t>(bestLag)]) {
                ++bestLag;
            }
            while (bestLag - 1 >= m_minLagSamples &&
                   m_cmnd[static_cast<std::size_t>(bestLag - 1)] <
                       m_cmnd[static_cast<std::size_t>(bestLag)]) {
                --bestLag;
            }
        }
    }

    // --- Step 4 (T1.6): parabolic interpolation ---------------------------
    //
    // bestLag is a whole number of samples, and a true period almost never
    // is. That quantisation is worst exactly where it matters most: at
    // 65 Hz a period is 738 samples and one sample of error is 2.3 cents,
    // but at 1100 Hz a period is only 43.6 samples and one sample is
    // 39 cents -- a quarter of a semitone. AC1 asks for +/-5.
    //
    // The fix is to stop reading d' as a list of values and read it as a
    // sampled curve. Around its minimum the curve is locally parabolic, so
    // fit a parabola through the chosen point and its two neighbours and
    // take that parabola's vertex. With the three samples at x = -1, 0, +1
    // carrying values s0, s1, s2, the parabola y = a*x^2 + b*x + c has
    //     a = (s0 + s2) / 2 - s1,   b = (s2 - s0) / 2
    // and its vertex sits at x = -b / (2a), which rearranges to the
    // expression below. This is YIN step 5 in the paper.
    //
    // Both neighbours are guaranteed to exist: bestLag >= m_minLagSamples
    // >= 1 gives a left neighbour, and prepare() computed one lag beyond
    // m_maxLagSamples precisely so the top of the range has a right one.
    const double left = m_cmnd[static_cast<std::size_t>(bestLag - 1)];
    const double centre = m_cmnd[static_cast<std::size_t>(bestLag)];
    const double right = m_cmnd[static_cast<std::size_t>(bestLag + 1)];

    double refinedLag = static_cast<double>(bestLag);

    // The parabola's curvature. It is positive at a genuine minimum; zero
    // or negative means these three points do not describe a dip (a flat
    // stretch, or numerically identical values on a perfectly periodic
    // synthetic signal), in which case there is nothing to interpolate and
    // the integer lag stands. Guarding this also keeps the division safe
    // (engine/CLAUDE.md).
    const double curvature = left - 2.0 * centre + right;
    if (curvature > 0.0) {
        const double shift = (left - right) / (2.0 * curvature);
        // A vertex more than half a sample away means the true minimum was
        // not the point we picked, so the parabola is being extrapolated
        // rather than interpolated. Clamp rather than trust it.
        refinedLag += std::min(0.5, std::max(-0.5, shift));
    }

    // Period (samples) -> frequency (Hz): f = sampleRate / lag. refinedLag
    // is within half a sample of bestLag >= m_minLagSamples >= 1, so this
    // never divides by zero.
    const float frequencyHz = static_cast<float>(m_sampleRate / refinedLag);

    // The aperiodicity of the lag actually being reported. On a frame that
    // step 3b promoted this is d' at the DOUBLED lag -- lower than the
    // value the voicing gate above was taken on, never higher, because the
    // promotion only fires when the doubled lag is markedly deeper. So the
    // reported confidence rises on repaired frames, which is the honest
    // answer: the signal really does repeat better there.
    const double aperiodicity = m_cmnd[static_cast<std::size_t>(bestLag)];

    // Confidence is the aperiodicity's complement, clamped because d' can
    // exceed 1 on a non-stationary window. Note it is not a loudness
    // meter: step 2 divides loudness out of d' entirely, so a sine at
    // 1/1000 of full scale is still reported voiced at full confidence.
    const float confidence = static_cast<float>(std::min(1.0, std::max(0.0, 1.0 - aperiodicity)));

    return PitchEstimate{frequencyHz, confidence, true};
}

} // namespace opentune
