#include "opentune/AutocorrelationDetector.h"

#include <algorithm>
#include <cmath>

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
    // from the maximum frequency, and vice versa. std::lround rounds to
    // the nearest integer sample count; std::max guards the degenerate
    // case (an absurdly high sample rate) where that could round to zero,
    // which would make "lag 0" -- comparing the signal to itself
    // unshifted, always a perfect match -- searchable, which is never a
    // meaningful pitch estimate.
    m_minLagSamples = std::max(
        1, static_cast<int>(std::lround(sampleRate / static_cast<double>(kMaxFrequencyHz))));
    m_maxLagSamples =
        static_cast<int>(std::lround(sampleRate / static_cast<double>(kMinFrequencyHz)));

    // The correlation search, for a given lag L, sums
    // buffer[i] * buffer[i + L] over a comparison window of
    // m_analysisWindowSamples samples. For the *largest* lag we try
    // (m_maxLagSamples) to have a full window of valid, in-bounds samples
    // to compare against, the buffer must hold at least
    // m_analysisWindowSamples + m_maxLagSamples samples in total. We size
    // the comparison window itself to kAnalysisWindowLagMultiple (2) times
    // the longest lag, so that even the lowest frequency in range (65 Hz,
    // the longest period) gets two full periods of signal inside the
    // comparison window -- one period is enough to find *a* peak, but a
    // second period makes that peak far less likely to be a fluke of a
    // single noisy cycle. At the nominal 48 kHz this works out to roughly
    // 2 * 738 = 1476 comparison samples plus 738 samples of lag headroom,
    // i.e. ~2214 samples total -- the same order of magnitude as the
    // "~2048 samples to see several periods of a low voice" figure this
    // task's brief uses to motivate needing a ring buffer at all.
    m_analysisWindowSamples = kAnalysisWindowLagMultiple * m_maxLagSamples;
    const int bufferSize = m_analysisWindowSamples + m_maxLagSamples;

    // Sized once, here, and never resized: process() only ever writes
    // into existing elements of m_buffer (constitution II, engine/CLAUDE.md
    // "Buffering -- this is the part that matters").
    m_buffer.assign(static_cast<std::size_t>(bufferSize), 0.0f);
    m_samplesFilled = 0;

    // One scratch slot per candidate lag (see the class comment on
    // m_correlationScratch for why the whole array is needed before a
    // lag can be chosen).
    const int lagCount = m_maxLagSamples - m_minLagSamples + 1;
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
    // For each candidate lag, sum buffer[i] * buffer[i + lag] across the
    // comparison window and stash it in m_correlationScratch. This is the
    // O(lags * window) cost the task brief calls out as "what pitch
    // detection actually costs" for the naive method -- YIN (Stage 1) does
    // the same shape of search but with a cheaper difference function and
    // an early-abort step.
    const int lagCount = m_maxLagSamples - m_minLagSamples + 1;
    for (int lagIndex = 0; lagIndex < lagCount; ++lagIndex) {
        const int lag = m_minLagSamples + lagIndex;
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
    // correlation[i-1] and correlation[i+1]. The two array endpoints
    // (m_minLagSamples and m_maxLagSamples) are deliberately never treated
    // as peaks here, even though one has no left neighbour and the other
    // no right: correlation is naturally high and still falling near the
    // shortest lags we search (a short shift barely decorrelates a smooth
    // signal from itself, however far its true period is), so the very
    // first scratch entry can easily be higher than its one visible
    // neighbour without being anywhere near a genuine period -- exactly
    // the false "peak" this loop must not report as the pitch. Real
    // periodic peaks always have signal on both sides showing the
    // correlation rising into them and falling back out; only an interior
    // index can show that.
    int bestLagIndex = -1;
    for (int lagIndex = 1; lagIndex < lagCount - 1; ++lagIndex) {
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
    // back to the single largest correlation sum found (e.g. a signal
    // whose correlation rises monotonically to the edge of the search
    // range rather than forming a clean interior peak). If even that best
    // candidate does not clear the threshold, this frame is unvoiced --
    // e.g. white noise, which has no lag anywhere near a perfect
    // self-match.
    if (bestLagIndex < 0) {
        for (int lagIndex = 0; lagIndex < lagCount; ++lagIndex) {
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

    const int bestLag = m_minLagSamples + bestLagIndex;

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
