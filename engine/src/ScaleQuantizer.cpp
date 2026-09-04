#include "opentune/ScaleQuantizer.h"

#include <cmath>

namespace opentune {

namespace {

// Tuning-standard reference used throughout this engine (engine/CLAUDE.md
// "Numerics"): concert pitch A4 is defined as 440 Hz, and by convention
// that note is assigned MIDI note number 69. Every Hz<->MIDI conversion
// below is anchored to this one pair of constants.
constexpr float kA4Hz = 440.0f;
constexpr float kA4MidiNote = 69.0f;

// Semitones per octave in twelve-tone equal temperament -- the exponent
// base for both directions of the Hz<->MIDI conversion below.
constexpr float kSemitonesPerOctave = 12.0f;

// Deliberately `float`, not `double`, throughout this file: engine/CLAUDE.md
// says frequencies live in `float`, snap()'s parameter and return type are
// both `float` (specs.md section 7.1), and -Wdouble-promotion would flag any
// stray `double` intermediate as a boundary needing a cast anyway. `float`'s
// ~7 decimal digits of precision is far more than this conversion needs: the
// whole pitch range of interest (65-1100 Hz, engine/CLAUDE.md) spans well
// under 100 fractional MIDI notes, and this engine's coarsest tolerance
// anywhere is single-digit cents (hundredths of a semitone) -- nowhere near
// float's precision floor. There is no accuracy reason to reach for `double`
// here, so the type stays `float` end to end.

} // namespace

ScaleQuantizer::ScaleQuantizer(Scale scale) noexcept : m_scale(scale) {}

void ScaleQuantizer::setScale(Scale scale) noexcept {
    m_scale = scale;
}

float ScaleQuantizer::snap(float frequencyHz) const noexcept {
    // frequencyHz <= 0 is not a pitch: by the PitchDetector.h convention,
    // 0 Hz (or, defensively, any non-positive value) means "no periodic
    // signal here" -- unvoiced audio, silence, breath. There is no nearest
    // musical pitch to a frequency that does not exist, and log2 of a
    // non-positive number is undefined/NaN, so this case is handled first
    // and explicitly: pass 0 straight through rather than quantizing it.
    if (frequencyHz <= 0.0f) {
        return 0.0f;
    }

    // Stage 0 placeholder: only ScaleType::Chromatic is implemented. Real
    // scale tables that restrict which semitones are allowed (Major,
    // NaturalMinor, HarmonicMinor, Pentatonic -- built from m_scale.type
    // and rooted at m_scale.rootMidiNote) are Stage 5 work. Until then,
    // every ScaleType value -- including Chromatic -- snaps to the
    // nearest equal-tempered semitone, so m_scale.type is intentionally
    // not branched on here.

    // Hz -> fractional MIDI note number: m = 69 + 12*log2(f/440)
    // (engine/CLAUDE.md Numerics). This is the inverse of the MIDI->Hz
    // formula used below: doubling the frequency raises the note by one
    // octave (12 semitones), and log2 turns that multiplicative relationship
    // into an additive one, which is what lets "round to nearest semitone"
    // just be "round to nearest integer" in this space.
    const float fractionalMidiNote =
        kA4MidiNote + kSemitonesPerOctave * std::log2(frequencyHz / kA4Hz);

    // Round to the nearest integer semitone. std::lround rounds
    // half-away-from-zero (so a value exactly 50 cents sharp of a semitone
    // rounds up to the next one, never down) and returns `long`; this
    // engine's frequency values live in `float`, so the result is cast
    // back down explicitly (-Wconversion demands it, per engine/CLAUDE.md).
    const long roundedMidiNoteLong = std::lround(fractionalMidiNote);
    const float nearestMidiNote = static_cast<float>(roundedMidiNoteLong);

    // MIDI -> Hz: f = 440 * 2^((m-69)/12) (engine/CLAUDE.md Numerics).
    return kA4Hz * std::exp2((nearestMidiNote - kA4MidiNote) / kSemitonesPerOctave);
}

} // namespace opentune
