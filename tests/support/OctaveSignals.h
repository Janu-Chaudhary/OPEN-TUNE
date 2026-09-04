// Synthetic signals designed to trigger octave errors in a pitch detector.
//
// The audio concept: a periodic sound at fundamental frequency f0 is not a
// pure tone. It is a *harmonic stack* -- energy at f0, 2*f0, 3*f0, 4*f0, and
// so on (integer multiples of f0, called "partials" or "harmonics"; the
// fundamental itself is "H1", 2*f0 is "H2", etc). The waveform's *period* is
// determined by f0 regardless of which harmonics are present or how loud
// they are, because every harmonic's own period divides evenly into f0's
// period (1/f0 seconds) -- so the whole sum repeats every 1/f0 seconds no
// matter what.
//
// But a detector does not know that mathematically; it measures periodicity
// empirically (see AutocorrelationDetector), and a stack with a weak or
// absent fundamental *also* looks strongly periodic at half that period
// (1/(2*f0), i.e. H2's own period) -- because if H1 is weak, nearly all of
// the signal's energy (H2, H4, H6, ...) genuinely repeats twice as often as
// the true period demands. A detector can lock onto that shorter, wrong
// period. This is the textbook "octave error", and it is *exactly* what
// human pitch perception gets right and naive algorithms get wrong: a
// listener hears the "missing fundamental" as f0 even when no energy exists
// there at all, because the auditory system infers it from the harmonic
// spacing rather than measuring raw periodicity the way autocorrelation
// does.
//
// This header builds known-f0 signals that stress that exact failure mode,
// for use against real detectors (see test_octave_signals.cpp). It is test
// support, not engine code: real-time rules do not apply here, and
// std::vector / header-only implementation are fine (see Signals.h's own
// header comment, which this file follows).
#pragma once

#include "Signals.h"

#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

namespace opentune::test {

// Frequencies spanning the vocal range this project cares about (specs.md
// section 6: 65-1100 Hz detectable range), from low male chest voice up
// through high female voice. The weak-fundamental octave-error problem
// documented above gets *worse* as f0 falls: a lower f0 packs its harmonics
// more densely relative to the fixed telephone-band and analysis-window
// constraints below, and low male voices are exactly where H2 dominating H1
// is common in natural speech (see h2DominantStack below). Covering this
// spread, rather than one frequency, is the point of this test set -- a
// detector that is fine at 440 Hz can still fail badly at 90 Hz.
inline constexpr float kLowMaleHz = 90.0f;     // low male chest voice
inline constexpr float kMaleHz = 130.0f;       // typical male speaking/singing
inline constexpr float kLowFemaleHz = 220.0f;  // low female / high male
inline constexpr float kMidHz = 330.0f;        // mixed range
inline constexpr float kA4Hz = 440.0f;         // concert pitch reference
inline constexpr float kHighMidHz = 600.0f;    // upper mixed range
inline constexpr float kHighFemaleHz = 800.0f; // high female voice

inline const std::vector<float>& vocalRangeTestFrequenciesHz() {
    static const std::vector<float> frequencies = {kLowMaleHz, kMaleHz,    kLowFemaleHz, kMidHz,
                                                   kA4Hz,      kHighMidHz, kHighFemaleHz};
    return frequencies;
}

// Converts a decibel ratio to a linear amplitude multiplier: 20*log10(ratio)
// inverted, i.e. `ratio = 10^(db/20)`. Used to express "the fundamental is
// 20 dB below H2" the way an audio engineer would state it, rather than as
// an opaque linear fraction.
inline float dbToLinearAmplitude(float db) {
    return static_cast<float>(std::pow(10.0, static_cast<double>(db) / 20.0));
}

// Sums sine components at f0, 2*f0, 3*f0, ... into one signal. `harmonicAmplitudes[i]`
// is the peak amplitude of harmonic number `i + 1` (index 0 = the fundamental H1,
// index 1 = H2, and so on) -- 0.0f means that harmonic is entirely absent, which is
// how the "missing fundamental" case below is built: not a filtered-out fundamental,
// simply one that was never added.
//
// Every harmonic at or above the Nyquist frequency (sampleRate / 2) is silently
// skipped rather than aliased -- an unaliased signal is what every case here needs to
// stay a clean, known-answer test.
inline std::vector<float> harmonicStack(float f0, double sampleRate, int numSamples,
                                        const std::vector<float>& harmonicAmplitudes) {
    std::vector<float> signal(static_cast<std::size_t>(numSamples), 0.0f);

    for (std::size_t h = 0; h < harmonicAmplitudes.size(); ++h) {
        const float amplitude = harmonicAmplitudes[h];
        if (amplitude == 0.0f) {
            continue;
        }

        const float harmonicNumber = static_cast<float>(h + 1);
        const float harmonicHz = f0 * harmonicNumber;
        if (static_cast<double>(harmonicHz) >= sampleRate / 2.0) {
            continue;
        }

        const std::vector<float> component = sine(harmonicHz, sampleRate, numSamples, amplitude);
        for (int n = 0; n < numSamples; ++n) {
            signal[static_cast<std::size_t>(n)] += component[static_cast<std::size_t>(n)];
        }
    }
    return signal;
}

// How many harmonics the stacks below carry above the fundamental slot. 8 partials
// is enough headroom that even the lowest test frequency (90 Hz) has several
// harmonics comfortably inside the detector's declared 65-1100 Hz range, and high
// test frequencies (800 Hz) still keep a couple of harmonics under Nyquist at 48 kHz.
inline constexpr int kDefaultHarmonicCount = 8;

// A natural-sounding harmonic series: amplitude 1/h for harmonic h (h = 1, 2, 3, ...).
// This is the classic sawtooth-like rolloff -- the fundamental (h=1) is always the
// loudest partial, which is exactly what a "normal", easy-to-detect voiced tone looks
// like. Used as the shared starting point for every stack below, and directly as the
// CONTROL case.
inline std::vector<float> naturalRolloffAmplitudes(int harmonicCount = kDefaultHarmonicCount) {
    std::vector<float> amplitudes(static_cast<std::size_t>(harmonicCount));
    for (int h = 0; h < harmonicCount; ++h) {
        amplitudes[static_cast<std::size_t>(h)] = 1.0f / static_cast<float>(h + 1);
    }
    return amplitudes;
}

// CONTROL CASE. A normal harmonic stack with a strong fundamental (the loudest
// partial, as in any naturally voiced tone). A working detector MUST get this right
// -- without a control, "the detector reported the wrong pitch" cannot be
// distinguished from "the signal itself was pathological". Every other case in this
// file changes exactly one thing relative to this one: how loud H1 is.
inline std::vector<float> controlStack(float f0, double sampleRate, int numSamples) {
    return harmonicStack(f0, sampleRate, numSamples, naturalRolloffAmplitudes());
}

// MISSING FUNDAMENTAL. Energy only at 2*f0, 3*f0, 4*f0, ... -- H1's amplitude is
// exactly 0.0f (never added, not filtered out). The true pitch is still f0: the
// waveform's period is unchanged by removing H1, because every remaining harmonic's
// period still divides evenly into 1/f0. Human hearing reconstructs f0 from harmonic
// *spacing* and reports it correctly -- this is the "missing fundamental" or "phantom
// fundamental" illusion, well documented in psychoacoustics (see e.g. Schouten,
// 1940s cochlear-nerve residue-pitch experiments). A detector that only measures raw
// periodicity, like autocorrelation, has no such spacing cue and is exactly the kind
// of signal decision 0003 predicts will fail.
inline std::vector<float> missingFundamentalStack(float f0, double sampleRate, int numSamples) {
    std::vector<float> amplitudes = naturalRolloffAmplitudes();
    amplitudes[0] = 0.0f; // H1 entirely absent.
    return harmonicStack(f0, sampleRate, numSamples, amplitudes);
}

// How far below H2 the fundamental sits in the "attenuated" case, in decibels. -20 dB
// is loud enough that H1 is still a real, present component (unlike the missing-
// fundamental case above) but quiet enough that it can be outweighed by the combined
// energy of the harmonics above it -- the intermediate point between "gone" and
// "normal".
inline constexpr float kAttenuatedFundamentalDb = -20.0f;

// ATTENUATED FUNDAMENTAL. Like the control stack, but H1 is turned down by
// kAttenuatedFundamentalDb relative to H2 rather than removed outright. This is the
// more realistic version of the missing-fundamental case: many real microphones,
// telephone codecs, and small speakers roll off bass response rather than eliminating
// it, leaving a present-but-weak fundamental.
inline std::vector<float> attenuatedFundamentalStack(float f0, double sampleRate, int numSamples) {
    std::vector<float> amplitudes = naturalRolloffAmplitudes();
    const float h2Amplitude = amplitudes[1]; // H2's amplitude under natural rolloff (1/2).
    amplitudes[0] = h2Amplitude * dbToLinearAmplitude(kAttenuatedFundamentalDb);
    return harmonicStack(f0, sampleRate, numSamples, amplitudes);
}

// H2-DOMINANT. The second harmonic, not the fundamental, is the single loudest
// partial. This is a common, entirely natural spectral shape -- not a pathological
// edge case -- in male vowels (formant frequencies can land closer to 2*f0 than f0
// for a low voice) and in telephone-band audio (see telephoneBandStack below, which
// this case's amplitude shape is designed to resemble even before band-limiting).
// H1 is present (unlike missingFundamentalStack) but is not the strongest partial the
// way it is in every other case here.
inline std::vector<float> h2DominantStack(float f0, double sampleRate, int numSamples) {
    std::vector<float> amplitudes = naturalRolloffAmplitudes();
    amplitudes[0] = 0.5f; // H1: present, but quieter than H2.
    amplitudes[1] = 1.0f; // H2: the loudest partial.
    return harmonicStack(f0, sampleRate, numSamples, amplitudes);
}

// EVEN-HARMONICS-ONLY. Not one of this task's five required cases -- added because
// measuring the five above (test_octave_signals.cpp) found they do NOT reproduce
// decision 0003's predicted octave-high failure on this detector: a weak, attenuated,
// or even entirely absent H1 still leaves odd harmonics (H3, H5, H7, ...) in the
// signal, and those odd harmonics destructively interfere at H2's own (half-period)
// lag -- see test_octave_signals.cpp's header comment for the worked-out correlation
// arithmetic. The true period's correlation is *exactly* 1.0 by construction for any
// integer-harmonic signal (shifting by one full period reproduces the identical
// waveform, however that period's energy is spread across harmonics), so a competing
// shorter-lag peak only threatens it when that shorter period is *itself* a genuine
// period of the whole signal -- which requires every harmonic present to be a
// multiple of 2 relative to f0, i.e. odd harmonics entirely absent, not just H1.
//
// This case is that condition: H1, H3, H5, H7 (every odd harmonic) are zero, and only
// H2, H4, H6, H8 remain. The signal is then, mathematically, exactly as periodic at
// f0/2's reciprocal lag (2*f0) as it is at f0 -- both lags achieve a normalised
// correlation of 1.0 -- so the first-peak search (shortest lag first) is genuinely
// tied, and decision 0003's predicted bias resolves that tie toward the shorter,
// wrong lag. Included so this test set contains at least one case that verifiably
// reproduces the documented failure mode, for T1.8 to compare YinDetector against
// later -- see docs/decisions/0003.
inline std::vector<float> evenHarmonicsOnlyStack(float f0, double sampleRate, int numSamples) {
    std::vector<float> amplitudes = naturalRolloffAmplitudes();
    for (std::size_t h = 0; h < amplitudes.size(); ++h) {
        const bool isOddHarmonic = ((h + 1) % 2) != 0; // h is 0-indexed; harmonic number is h+1.
        if (isOddHarmonic) {
            amplitudes[h] = 0.0f;
        }
    }
    return harmonicStack(f0, sampleRate, numSamples, amplitudes);
}

// The telephone band (specs.md does not set this; it is a long-standing telephony
// convention -- ITU-T G.711 narrowband voice channels pass roughly 300-3400 Hz and
// filter out everything else, by design, to fit many calls into limited bandwidth).
inline constexpr float kTelephoneBandLowHz = 300.0f;
inline constexpr float kTelephoneBandHighHz = 3400.0f;

// BAND-LIMITED (telephone-band). Only harmonics that fall inside
// [kTelephoneBandLowHz, kTelephoneBandHighHz] are present at all; every harmonic
// outside that band -- including f0 itself, for every frequency in
// vocalRangeTestFrequenciesHz() below kTelephoneBandLowHz -- has zero energy. This
// models what a real telephone/VoIP band-pass filter does: it does not attenuate the
// fundamental, it removes it completely, along with every other out-of-band harmonic.
// Built directly from which harmonics survive the band, rather than by actually
// filtering a full-band stack, because for a discrete sum of sines the two are
// equivalent: an ideal brick-wall filter passes an in-band sine unchanged and a
// out-of-band sine not at all, which is exactly "include this harmonic" / "don't".
inline std::vector<float> telephoneBandStack(float f0, double sampleRate, int numSamples,
                                             int harmonicCount = kDefaultHarmonicCount) {
    const std::vector<float> fullBand = naturalRolloffAmplitudes(harmonicCount);
    std::vector<float> amplitudes(static_cast<std::size_t>(harmonicCount), 0.0f);

    for (int h = 0; h < harmonicCount; ++h) {
        const float harmonicHz = f0 * static_cast<float>(h + 1);
        if (harmonicHz >= kTelephoneBandLowHz && harmonicHz <= kTelephoneBandHighHz) {
            amplitudes[static_cast<std::size_t>(h)] = fullBand[static_cast<std::size_t>(h)];
        }
    }
    return harmonicStack(f0, sampleRate, numSamples, amplitudes);
}

// One octave-error test case: a signal with a KNOWN true f0, paired with a name
// describing which weak/missing-fundamental scenario it exercises. Every case in
// generateOctaveErrorCases() below carries the true f0 alongside the generated
// signal so a test can measure error against it directly, rather than trusting the
// generator was called correctly at the call site.
struct OctaveCase {
    std::string name;
    float trueF0Hz;
    std::vector<float> signal;
};

// Builds the full octave-error test set described in this header's comment: for
// every frequency across the vocal range (vocalRangeTestFrequenciesHz), the control
// case plus every weak/missing-fundamental scenario -- and, additionally, the
// telephone-band case only for frequencies whose fundamental actually falls below
// kTelephoneBandLowHz (300 Hz), since above that the case would trivially include an
// unfiltered fundamental and would not be exercising anything.
inline std::vector<OctaveCase> generateOctaveErrorCases(double sampleRate, int numSamples) {
    std::vector<OctaveCase> cases;

    for (const float f0 : vocalRangeTestFrequenciesHz()) {
        cases.push_back({"control@" + std::to_string(static_cast<int>(f0)), f0,
                         controlStack(f0, sampleRate, numSamples)});
        cases.push_back({"missingFundamental@" + std::to_string(static_cast<int>(f0)), f0,
                         missingFundamentalStack(f0, sampleRate, numSamples)});
        cases.push_back({"attenuatedFundamental@" + std::to_string(static_cast<int>(f0)), f0,
                         attenuatedFundamentalStack(f0, sampleRate, numSamples)});
        cases.push_back({"h2Dominant@" + std::to_string(static_cast<int>(f0)), f0,
                         h2DominantStack(f0, sampleRate, numSamples)});
        cases.push_back({"evenHarmonicsOnly@" + std::to_string(static_cast<int>(f0)), f0,
                         evenHarmonicsOnlyStack(f0, sampleRate, numSamples)});

        if (f0 < kTelephoneBandLowHz) {
            cases.push_back({"telephoneBand@" + std::to_string(static_cast<int>(f0)), f0,
                             telephoneBandStack(f0, sampleRate, numSamples)});
        }
    }
    return cases;
}

} // namespace opentune::test
