#include "opentune/AutocorrelationDetector.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace opentune {

void AutocorrelationDetector::prepare(double sampleRate, int maxBlockSize) {
    // maxBlockSize does not change this detector's sizing: the ring buffer
    // is shifted and refilled by however many samples process() receives
    // each call (see process() below), so it works correctly for any
    // n <= maxBlockSize without needing to know maxBlockSize up front.
    // Kept as a parameter only because the PitchDetector interface
    // requires it (specs.md section 7.1).
    (void)maxBlockSize;

    m_sampleRate = sampleRate;

    // Sample-domain equivalents of the detectable pitch range (specs.md
    // section 6: 65 Hz - 1100 Hz). A higher frequency corresponds to a
    // *shorter* period, hence a *smaller* lag -- so the minimum lag comes
    // from the maximum frequency, and vice versa.
    //
    // These *widen* toward the requested range rather than rounding to the
    // nearest sample, so the declared 65-1100 Hz range is genuinely
    // covered end to end, not just approximately: static_cast<int> of a
    // positive double truncates toward zero, i.e. rounds down, so
    // m_minLagSamples (whose *smaller* value gives a *higher* implied
    // frequency ceiling) is deliberately the smallest lag that still
    // covers kMaxFrequencyHz, not the nearest one -- e.g. at 48 kHz,
    // truncating gives lag 43 (ceiling 1116.3 Hz) rather than rounding to
    // lag 44 (ceiling only 1090.9 Hz, short of the declared 1100 Hz).
    // Symmetrically, std::ceil on m_maxLagSamples's division gives the
    // smallest lag whose implied frequency floor is <= kMinFrequencyHz.
    // std::max guards the degenerate case (an absurdly high sample rate)
    // where truncation could give zero, which would make "lag 0" --
    // comparing the signal to itself unshifted, always a perfect match --
    // searchable, which is never a meaningful pitch estimate.
    m_minLagSamples =
        std::max(1, static_cast<int>(sampleRate / static_cast<double>(kMaxFrequencyHz)));
    m_maxLagSamples =
        static_cast<int>(std::ceil(sampleRate / static_cast<double>(kMinFrequencyHz)));

    // The correlation search, for a given lag L, sums
    // buffer[i] * buffer[i + L] over a comparison window of
    // m_analysisWindowSamples samples. We size the comparison window
    // itself to kAnalysisWindowLagMultiple (2) times the longest lag, so
    // that even the lowest frequency in range (65 Hz, the longest period)
    // gets two full periods of signal inside the comparison window -- one
    // period is enough to find *a* peak, but a second period makes that
    // peak far less likely to be a fluke of a single noisy cycle. At the
    // nominal 48 kHz this works out to roughly 2 * 739 = 1478 comparison
    // samples -- the same order of magnitude as the "~2048 samples to see
    // several periods of a low voice" figure this task's brief uses to
    // motivate needing a ring buffer at all.
    m_analysisWindowSamples = kAnalysisWindowLagMultiple * m_maxLagSamples;

    // The scratch array (below) computes correlation sums for lags
    // [m_correlationBaseLag, m_maxLagSamples + 1] -- one lag further on
    // each end than the declared answer range -- so that both
    // m_minLagSamples and m_maxLagSamples land on an *interior* scratch
    // index with a real neighbour on both sides (see the class comment on
    // m_correlationBaseLag for why that matters). For the buffer to have a
    // full, in-bounds comparison window even at that widest lag
    // (m_maxLagSamples + 1), it must hold
    // m_analysisWindowSamples + m_maxLagSamples + 1 samples in total.
    m_correlationBaseLag = std::max(1, m_minLagSamples - 1);
    const int correlationTopLag = m_maxLagSamples + 1;
    const int bufferSize = m_analysisWindowSamples + correlationTopLag;

    // Sized once, here, and never resized: process() only ever writes
    // into existing elements of m_buffer (constitution II, engine/CLAUDE.md
    // "Buffering -- this is the part that matters").
    m_buffer.assign(static_cast<std::size_t>(bufferSize), 0.0f);
    m_samplesFilled = 0;

    // One scratch slot per lag in [m_correlationBaseLag, correlationTopLag]
    // (see the class comment on m_correlationScratch for why the whole
    // array, including the one or two padding entries beyond the declared
    // answer range, is needed before a lag can be chosen).
    const int lagCount = correlationTopLag - m_correlationBaseLag + 1;
    m_correlationScratch.assign(static_cast<std::size_t>(lagCount), 0.0);
}

void AutocorrelationDetector::reset() noexcept {
    // std::fill over m_buffer is bounded, fixed-size work (the buffer was
    // sized once in prepare() and never grows) -- not the kind of
    // input-dependent-cost operation constitution II bans. It puts the
    // buffer back to its post-prepare() all-zero state so a stale, partly
    // filled window from before a reset can never leak into the next
    // process() call's analysis.
    std::fill(m_buffer.begin(), m_buffer.end(), 0.0f);
    m_samplesFilled = 0;
}

PitchEstimate AutocorrelationDetector::process(const float* block, int n) noexcept {
    // Not prepared (or prepared with a degenerate buffer): nothing to
    // analyse. Defensive only -- prepare() is always called before
    // streaming per the PitchDetector contract.
    const int bufferSize = static_cast<int>(m_buffer.size());
    if (bufferSize == 0 || n <= 0) {
        return PitchEstimate{0.0f, 0.0f, false};
    }

    // --- Append this block to the ring buffer -----------------------------
    //
    // Implemented as "shift left, append at the tail" rather than a
    // circular (wrap-around) index. This is still real-time safe: both
    // std::copy calls below touch at most bufferSize elements of an
    // already-allocated std::vector, so the cost is bounded by a constant
    // fixed at prepare() time, not by anything unbounded. The payoff is
    // that the analysis below can treat m_buffer as one contiguous,
    // chronologically-ordered window with no index arithmetic to get
    // wrong -- worth the extra copy at this detector's block sizes.
    if (n >= bufferSize) {
        // This block alone is at least a full window: only its most
        // recent bufferSize samples matter, and they entirely replace
        // whatever was buffered before (no lookahead is violated here --
        // we are simply discarding samples older than the window, not
        // reading ahead of sample n-1).
        std::copy(block + (n - bufferSize), block + n, m_buffer.begin());
        m_samplesFilled = bufferSize;
    } else {
        // Discard the oldest n samples (shift everything else left by n)...
        std::copy(m_buffer.begin() + n, m_buffer.end(), m_buffer.begin());
        // ...then append the new block in the freed space at the tail.
        std::copy(block, block + n, m_buffer.end() - n);
        m_samplesFilled = std::min(bufferSize, m_samplesFilled + n);
    }

    // Buffer has not filled once yet: analysing it now would mean
    // correlating real signal against the zeros it was initialised with,
    // which is not a meaningful window. Per the brief: return unvoiced,
    // not a guess.
    if (m_samplesFilled < bufferSize) {
        return PitchEstimate{0.0f, 0.0f, false};
    }

    // --- Zero-lag energy and silence gate ----------------------------------
    //
    // r(0) (the correlation sum at lag 0, i.e. sum of buffer[i]^2 over the
    // comparison window) is both a signal-energy measure and the
    // normalising denominator used below to turn a raw correlation sum
    // into a 0..1 "how periodic is this" strength.
    const int windowSamples = m_analysisWindowSamples;
    double zeroLagEnergy = 0.0;
    for (int i = 0; i < windowSamples; ++i) {
        const double sample = static_cast<double>(m_buffer[static_cast<std::size_t>(i)]);
        zeroLagEnergy += sample * sample;
    }

    const double meanSquare = zeroLagEnergy / static_cast<double>(windowSamples);
    const double rms = std::sqrt(meanSquare);

    // kMinRmsForVoiced = 0.01: with this engine's nominal +/-1.0 signal
    // range, -40 dBFS RMS. Digital silence (this detector's silence test)
    // has rms == 0.0 exactly, so any positive threshold rejects it; 0.01 is
    // chosen to sit well below a genuinely sung or spoken note (a full-scale
    // sine's RMS is ~0.71) while still being far enough above the
    // near-zero rounding noise a quiet-but-real signal could have. This is
    // a simple, defensible placeholder -- T1.7 replaces it with a real
    // voiced/unvoiced classifier.
    if (rms < static_cast<double>(kMinRmsForVoiced)) {
        return PitchEstimate{0.0f, 0.0f, false};
    }

    // --- Autocorrelation search over the candidate lag range ---------------
    //
    // For each lag from m_correlationBaseLag up to m_maxLagSamples + 1
    // (the declared answer range [m_minLagSamples, m_maxLagSamples] plus
    // one padding lag on each end -- see the m_correlationBaseLag class
    // comment), sum buffer[i] * buffer[i + lag] across the comparison
    // window and stash it in m_correlationScratch. This is the
    // O(lags * window) cost the task brief calls out as "what pitch
    // detection actually costs" for the naive method (~1.0 million
    // multiply-adds per call at 48 kHz nominal sizing: roughly
    // (m_maxLagSamples - m_minLagSamples) * m_analysisWindowSamples =
    // ~696 lags * ~1478 samples) -- YIN (Stage 1) does the same shape of
    // search but with a cheaper difference function and an early-abort
    // step.
    const int lagCount = static_cast<int>(m_correlationScratch.size());
    for (int lagIndex = 0; lagIndex < lagCount; ++lagIndex) {
        const int lag = m_correlationBaseLag + lagIndex;
        double correlation = 0.0;
        for (int i = 0; i < windowSamples; ++i) {
            const double a = static_cast<double>(m_buffer[static_cast<std::size_t>(i)]);
            const double b = static_cast<double>(m_buffer[static_cast<std::size_t>(i + lag)]);
            correlation += a * b;
        }
        m_correlationScratch[static_cast<std::size_t>(lagIndex)] = correlation;
    }

    // --- Lag selection: first qualifying peak, not the global maximum ------
    //
    // A genuinely periodic signal's correlation sum is *itself*
    // periodic in lag: it peaks not just at the true period, but at every
    // integer multiple of it too (two periods of overlap correlate just as
    // well as one, three just as well as two, and so on -- this detector's
    // buffer is deliberately sized so no lag's comparison window shrinks,
    // see prepare(), which means those repeat peaks do not fade out with
    // distance the way they would against a naturally decaying real
    // instrument tone). Picking the single largest correlation sum in the
    // whole search range is therefore a coin flip among the true period and
    // all of its multiples -- for a clean, sustained tone they can be
    // separated by only tiny floating-point margins, and the true period is
    // not reliably the winner.
    //
    // The standard fix (and still squarely "naive autocorrelation", not a
    // YIN technique) is to walk lags from short to long and take the
    // *first* local peak whose normalised strength clears the voicing
    // threshold, rather than searching for the biggest peak anywhere in
    // range. The true period is always the first strong peak encountered;
    // its later multiples are only ever additional, redundant peaks after
    // it. This resolves the tie in favour of the shortest (correct) lag.
    //
    // "Local peak" here means an *interior* index i (both neighbours
    // present in the scratch array) where correlation[i] is >= both
    // correlation[i-1] and correlation[i+1]. Only indices whose lag falls
    // in the declared answer range [m_minLagSamples, m_maxLagSamples] are
    // ever eligible to be selected -- searchStartIndex/searchEndIndex
    // below are exactly that range's position in m_correlationScratch.
    // Thanks to prepare() padding the scratch array by one extra lag on
    // each end (m_correlationBaseLag, m_maxLagSamples + 1), both
    // m_minLagSamples and m_maxLagSamples normally land on a genuine
    // interior index with a real neighbour on both sides -- the padding
    // entries themselves are the only indices ever excluded from
    // candidacy by construction (std::max(1, ...) below only bites in the
    // degenerate case m_correlationBaseLag == m_minLagSamples, i.e. an
    // absurdly high sample rate where m_minLagSamples is already 1).
    //
    // This matters because correlation is naturally high and still
    // falling near the shortest lags we search (a short shift barely
    // decorrelates a smooth signal from itself, however far its true
    // period is) -- so a lag right at the edge of the search range can
    // easily be higher than the *one* neighbour visible to it without
    // being anywhere near a genuine period. Requiring a real neighbour on
    // both sides is what rules that false "peak" out. Real periodic peaks
    // always have signal on both sides showing the correlation rising
    // into them and falling back out; only a true interior index can show
    // that.
    const int searchStartIndex = std::max(1, m_minLagSamples - m_correlationBaseLag);
    const int searchEndIndex = std::min(lagCount - 2, m_maxLagSamples - m_correlationBaseLag);
    int bestLagIndex = -1;
    for (int lagIndex = searchStartIndex; lagIndex <= searchEndIndex; ++lagIndex) {
        const double value = m_correlationScratch[static_cast<std::size_t>(lagIndex)];
        const double left = m_correlationScratch[static_cast<std::size_t>(lagIndex - 1)];
        const double right = m_correlationScratch[static_cast<std::size_t>(lagIndex + 1)];
        if (value < left || value < right) {
            continue;
        }
        const double normalized = value / zeroLagEnergy;
        if (normalized >= static_cast<double>(kMinNormalizedCorrelation)) {
            bestLagIndex = lagIndex;
            break;
        }
    }

    // No local peak anywhere in range cleared the voicing threshold: fall
    // back to the single largest correlation sum found within the declared
    // answer range (e.g. a signal whose correlation rises monotonically to
    // the edge of the range rather than forming a clean interior peak).
    // The padding entries outside [searchStartIndex, searchEndIndex] are
    // excluded here too -- they exist only to give the true endpoints
    // neighbours, never to be answers themselves. If even the best
    // in-range candidate does not clear the threshold, this frame is
    // unvoiced -- e.g. white noise, which has no lag anywhere near a
    // perfect self-match.
    if (bestLagIndex < 0) {
        for (int lagIndex = searchStartIndex; lagIndex <= searchEndIndex; ++lagIndex) {
            if (bestLagIndex < 0 ||
                m_correlationScratch[static_cast<std::size_t>(lagIndex)] >
                    m_correlationScratch[static_cast<std::size_t>(bestLagIndex)]) {
                bestLagIndex = lagIndex;
            }
        }
    }

    const double bestCorrelation = m_correlationScratch[static_cast<std::size_t>(bestLagIndex)];

    // Normalise the winning lag's correlation sum against the zero-lag
    // energy: at lag 0 a signal always correlates perfectly with itself
    // (normalised == 1.0), so this ratio measures how close the winning
    // lag comes to that perfect self-match -- i.e. how strongly periodic
    // the signal actually is at that lag, independent of its absolute
    // loudness. zeroLagEnergy > 0 is already guaranteed here by the RMS
    // gate above (rms >= kMinRmsForVoiced > 0), so this division is safe.
    //
    // Note this denominator is r(0) of the comparison window alone, not
    // the geometric mean sqrt(r_a(0) * r_b(0)) of the two *shifted*
    // segments actually being compared (buffer[0..window) and
    // buffer[lag..window+lag)) that a stricter normalised
    // cross-correlation would use. Those two segments overlap almost
    // entirely for this detector's lags (lag is small relative to
    // windowSamples), so r(0) is a close enough stand-in for a stationary
    // signal, and it is far cheaper (one energy sum, computed once,
    // instead of one per candidate lag). The cost: for strongly
    // non-stationary input this ratio is not mathematically bounded to
    // [0, 1] the way a true normalised cross-correlation would be -- hence
    // the explicit clamp on `confidence` below. Good enough for this
    // Stage 0 placeholder; not a claim of a rigorous normalised statistic.
    const double normalizedCorrelation = bestCorrelation / zeroLagEnergy;

    // kMinNormalizedCorrelation = 0.6: a genuine sine or voiced tone's
    // best lag lands very close to a perfect self-match (normalised
    // correlation close to 1.0, typically > 0.95 for a clean sine even
    // with this detector's integer-lag quantization). Uncorrelated noise,
    // by contrast, has no lag where shifted-and-original line up any
    // better than chance, so its best achievable normalised correlation
    // sits far below this line (order 1/sqrt(windowSamples), a few percent
    // for this detector's window sizes). 0.6 sits well clear of both,
    // giving margin against a quiet or slightly noisy real voice without
    // being fooled by noise. Like the RMS threshold above, this is a
    // simple placeholder rule -- T1.7 replaces it.
    if (normalizedCorrelation < static_cast<double>(kMinNormalizedCorrelation)) {
        return PitchEstimate{0.0f, 0.0f, false};
    }

    // bestLagIndex is an index into m_correlationScratch, which starts at
    // m_correlationBaseLag (not necessarily m_minLagSamples -- see the
    // class comment on m_correlationBaseLag), so the lag it names is
    // recovered relative to that base.
    const int bestLag = m_correlationBaseLag + bestLagIndex;

    // Period (in samples) -> frequency (in Hz): f = sampleRate / lag.
    const float frequencyHz = static_cast<float>(m_sampleRate / static_cast<double>(bestLag));

    // Report the normalised correlation strength itself as confidence,
    // clamped to [0, 1]: it is already a 0..1-ish "how periodic" measure,
    // and clamping guards the rare case (a very short or unusual window)
    // where floating-point rounding could push it fractionally above 1.0.
    const float confidence =
        static_cast<float>(std::min(1.0, std::max(0.0, normalizedCorrelation)));

    return PitchEstimate{frequencyHz, confidence, true};
}

} // namespace opentune
