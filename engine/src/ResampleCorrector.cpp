#include "opentune/ResampleCorrector.h"

#include <algorithm>
#include <cmath>

namespace opentune {

void ResampleCorrector::prepare(double sampleRate, int maxBlockSize) {
    m_sampleRate = sampleRate;

    // See the kHistoryBlocks comment in the header for why this multiple
    // was chosen. std::max guards a degenerate maxBlockSize (<= 0).
    m_historyCapacity = kHistoryBlocks * std::max(1, maxBlockSize);

    // Sized once, here, and never resized: process() only ever writes into
    // existing elements of m_history (constitution II).
    m_history.assign(static_cast<std::size_t>(m_historyCapacity), 0.0f);

    m_samplesReceived = 0;
    m_readPos = 0.0;
}

void ResampleCorrector::reset() noexcept {
    // std::fill over m_history is bounded, fixed-size work (sized once in
    // prepare() and never grown) -- not the kind of input-dependent-cost
    // operation constitution II bans.
    std::fill(m_history.begin(), m_history.end(), 0.0f);
    m_samplesReceived = 0;
    m_readPos = 0.0;
}

void ResampleCorrector::process(const float* in, float* out, int n, float pitchRatio) noexcept {
    if (n <= 0) {
        return;
    }

    // Index (in the "one long stream of every sample ever received" space
    // described in the header) of in[0], the first sample of this block.
    const std::int64_t blockStart = m_samplesReceived;

    // Index of the newest sample this instance has actually been given as
    // of this call: the last sample of the block just handed to us. Any
    // read position beyond this is a request for a sample that has not
    // arrived yet -- exactly the "read position runs past available
    // input" case the task brief calls out. We hold (clamp to this index)
    // rather than emit silence or nothing: emitting silence would carve a
    // sharp discontinuity into the middle of an otherwise-still-sounding
    // tone, which is no more correct and gives an uglier, harder-to-reason
    // -about waveform than simply holding the last real sample we have.
    const std::int64_t newestAvailable = blockStart + static_cast<std::int64_t>(n) - 1;

    // Oldest sample still available to us at all: either sample 0 (we have
    // not yet received m_historyCapacity samples in total) or the oldest
    // sample still retained in m_history. A read position older than this
    // asks for a sample this bounded-size instance has already discarded
    // -- see the "history buffer" paragraph of the class comment. We hold
    // (clamp) here too, for the same reason as above.
    const std::int64_t oldestAvailable =
        std::max(std::int64_t{0}, blockStart - static_cast<std::int64_t>(m_historyCapacity));

    // Fetches the sample at stream index `index`, which the caller has
    // already clamped into [oldestAvailable, newestAvailable]. An index
    // >= blockStart falls within the block being processed right now, and
    // is read directly from `in` -- not a lookahead violation, since the
    // whole block was already handed to this call synchronously
    // (constitution III only forbids reading a sample that has not
    // actually arrived from the caller yet). An index < blockStart falls
    // in m_history, which holds the most recent m_historyCapacity samples
    // received before this block.
    const auto sampleAt = [&](std::int64_t index) noexcept -> float {
        if (index >= blockStart) {
            const std::int64_t offsetIntoBlock = index - blockStart;
            return in[static_cast<std::size_t>(offsetIntoBlock)];
        }
        // How far back from the start of this block. Guaranteed in
        // [1, m_historyCapacity] because the caller clamped `index` to
        // >= oldestAvailable before calling sampleAt().
        const std::int64_t stepsBack = blockStart - index;
        const std::size_t historyIndex =
            static_cast<std::size_t>(m_historyCapacity) - static_cast<std::size_t>(stepsBack);
        return m_history[historyIndex];
    };

    const double ratio = static_cast<double>(pitchRatio);
    double pos = m_readPos;

    for (int i = 0; i < n; ++i) {
        // Split `pos` into an integer sample index and a fractional
        // in-between position. std::floor (not truncation) matters here
        // once `pos` can be negative (it cannot be, in practice -- m_readPos
        // starts at 0.0 and pitchRatio is never negative in this engine's
        // contract -- but std::floor costs nothing extra and removes the
        // need to reason about truncation-toward-zero edge cases).
        const double posFloor = std::floor(pos);
        std::int64_t indexA = static_cast<std::int64_t>(posFloor);
        std::int64_t indexB = indexA + 1;
        const double frac = pos - posFloor;

        // Clamp both neighbouring indices into the available range. This
        // is where "hold" happens: once indexA (or indexB) would exceed
        // newestAvailable, it is clamped down to it instead, so the
        // interpolation below reads the same (most recent) sample
        // repeatedly rather than reading unarrived input. Symmetrically
        // for oldestAvailable when the read position has drifted behind
        // the retained history.
        indexA = std::clamp(indexA, oldestAvailable, newestAvailable);
        indexB = std::clamp(indexB, oldestAvailable, newestAvailable);

        float sample;
        if (frac == 0.0) {
            // Exact-integer read position: take sample A directly rather
            // than routing it through the interpolation formula below.
            // This is what makes pitchRatio == 1.0 bit-identical
            // passthrough (see header): with ratio exactly 1.0, `pos`
            // advances by exactly 1.0 every sample and stays an exact
            // integer forever, so this branch is always taken and every
            // output sample is a direct, unmodified copy of its input
            // sample -- no floating-point rounding from a multiply-add
            // formula can enter the picture, which `a*(1-frac) + b*frac`
            // is not otherwise guaranteed to avoid bit-for-bit under every
            // compiler and optimisation setting even when frac is
            // mathematically zero.
            sample = sampleAt(indexA);
        } else {
            // Linear interpolation: the two lines' worth of DSP this
            // entire "naive corrector" comes down to. `frac` is how far
            // (0..1) the true read position sits past sample A on its way
            // to sample B; the output is their weighted average, which
            // approximates the (unknown, in-between) value the original
            // continuous waveform would have had at that fractional
            // position. This is the cheapest possible interpolation
            // (a straight line between two points) -- and, not
            // coincidentally, the reason this corrector's pitch-shifted
            // output has more high-frequency distortion than a real
            // resampler using a longer (e.g. sinc) interpolation kernel
            // would. That inaccuracy is not a bug either: a "better"
            // interpolator would not fix the two documented problems this
            // class exists to demonstrate (duration change, formant
            // shift), so there is no reason to spend more CPU on one here.
            const float a = sampleAt(indexA);
            const float b = sampleAt(indexB);
            const float fracF = static_cast<float>(frac);
            sample = a * (1.0f - fracF) + b * fracF;
        }

        out[static_cast<std::size_t>(i)] = sample;
        pos += ratio;
    }

    m_readPos = pos;

    // --- Update history for the next call ----------------------------------
    //
    // m_history must now hold the most recent m_historyCapacity samples as
    // of the *end* of this block (i.e. including everything in `in`).
    // Same "shift left, append at the tail" pattern as
    // AutocorrelationDetector.cpp, and real-time safe for the same reason:
    // both branches touch at most m_historyCapacity elements, a constant
    // fixed at prepare() time, regardless of n.
    if (n >= m_historyCapacity) {
        // This block alone is at least a full history's worth: only its
        // most recent m_historyCapacity samples matter, and they entirely
        // replace whatever was retained before.
        std::copy(in + (n - m_historyCapacity), in + n, m_history.begin());
    } else {
        // Discard the oldest n samples (shift everything else left by
        // n)...
        std::copy(m_history.begin() + n, m_history.end(), m_history.begin());
        // ...then append this block in the freed space at the tail.
        std::copy(in, in + n, m_history.end() - n);
    }

    m_samplesReceived = blockStart + static_cast<std::int64_t>(n);
}

int ResampleCorrector::latencySamples() const noexcept {
    // See the class comment in the header: this implementation never
    // withholds a sample to build up a fixed lookahead window, so it
    // introduces no additional delay.
    return 0;
}

} // namespace opentune
