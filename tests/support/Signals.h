// Synthetic known-answer signal generators for tests.
//
// Every later DSP test (pitch detection, quantization, correction) measures its
// output against one of these generators, so their correctness is foundational:
// if `sine()` is subtly wrong, every downstream "detects 440 Hz within X cents"
// test is meaningless. Keep these simple and easy to verify by hand.
//
// This is test support, not engine code: real-time rules (no allocation, no
// exceptions in hot paths) do not apply here. `std::vector` and header-only
// implementation are fine.
#pragma once

#include <cmath>
#include <cstddef>
#include <random>
#include <vector>

namespace opentune::test {

// A generous, well-known constant: 2*pi. Used to convert a frequency in Hz
// into an angular phase increment per sample.
inline constexpr double kTwoPi = 6.283185307179586476925286766559;

// Generates `numSamples` of a pure sine wave at `hz`, sampled at `sampleRate`,
// scaled to `amplitude` peak. Sample n is `amplitude * sin(2*pi*hz*n/sampleRate)`.
//
// Used as the primary known-answer signal: a detector reading this signal
// should report `hz` (within its own tolerance), and its peak should be
// `amplitude`, and it should cross zero `2 * hz * duration` times.
inline std::vector<float> sine(float hz, double sampleRate, int numSamples,
                               float amplitude = 1.0f) {
    std::vector<float> signal(static_cast<std::size_t>(numSamples));

    // Angular frequency: radians of phase advanced per sample.
    const double angularFrequency = kTwoPi * static_cast<double>(hz) / sampleRate;

    for (int n = 0; n < numSamples; ++n) {
        const double phase = angularFrequency * static_cast<double>(n);
        signal[static_cast<std::size_t>(n)] = amplitude * static_cast<float>(std::sin(phase));
    }
    return signal;
}

// Generates `numSamples` of digital silence (all-zero signal). Used to verify
// that a detector correctly reports "no pitch" / `voiced = false` on input
// with no energy, rather than latching onto noise or a stale estimate.
inline std::vector<float> silence(int numSamples) {
    return std::vector<float>(static_cast<std::size_t>(numSamples), 0.0f);
}

// Generates `numSamples` of white noise in [-1, 1], seeded with `seed` so the
// output is fully deterministic and reproducible across test runs and
// machines (a `std::mt19937` with a fixed seed always produces the same
// sequence, unlike a hardware or time-seeded generator).
//
// Used to verify a detector does NOT report a stable pitch on unpitched
// input — white noise has no dominant periodicity.
inline std::vector<float> whiteNoise(int numSamples, unsigned seed) {
    std::vector<float> signal(static_cast<std::size_t>(numSamples));

    std::mt19937 generator(seed);
    std::uniform_real_distribution<float> distribution(-1.0f, 1.0f);

    for (int n = 0; n < numSamples; ++n) {
        signal[static_cast<std::size_t>(n)] = distribution(generator);
    }
    return signal;
}

// Generates `numSamples` of a linear frequency sweep from `startHz` to
// `endHz`, sampled at `sampleRate`. The instantaneous frequency changes
// linearly with time across the whole buffer.
//
// Naively computing `sin(2*pi*f(t)*t)` with a time-varying f(t) produces the
// wrong waveform (and can produce audible discontinuities), because the
// phase of a swept sine is the *integral* of instantaneous frequency over
// time, not frequency times time. So instead we accumulate phase sample by
// sample: at each sample we know the instantaneous frequency, and we add
// `2*pi*f(n)/sampleRate` radians to a running phase total. This guarantees
// phase continuity (no jumps between samples) by construction, which is
// exactly the property later tests rely on ("no discontinuities").
//
// Reference: this is the standard "phase accumulator" method for generating
// a chirp/sweep signal; see e.g. https://en.wikipedia.org/wiki/Chirp
// ("Numerical chirp generation" via phase accumulation).
inline std::vector<float> sweep(float startHz, float endHz, double sampleRate, int numSamples) {
    std::vector<float> signal(static_cast<std::size_t>(numSamples));

    const double start = static_cast<double>(startHz);
    const double end = static_cast<double>(endHz);
    const double duration = static_cast<double>(numSamples) / sampleRate;

    double phase = 0.0;
    for (int n = 0; n < numSamples; ++n) {
        // Fraction of the way through the sweep, 0 at n=0 to ~1 at the last sample.
        const double t = static_cast<double>(n) / sampleRate;
        const double fractionElapsed = (duration > 0.0) ? (t / duration) : 0.0;

        // Instantaneous frequency at this sample: linear interpolation between
        // startHz and endHz.
        const double instantaneousHz = start + (end - start) * fractionElapsed;

        signal[static_cast<std::size_t>(n)] = static_cast<float>(std::sin(phase));

        // Accumulate phase by the angular frequency implied by the
        // instantaneous frequency at this sample. This running sum is what
        // makes the sweep phase-continuous rather than recomputed per-sample
        // from f(t)*t.
        phase += kTwoPi * instantaneousHz / sampleRate;
    }
    return signal;
}

} // namespace opentune::test
