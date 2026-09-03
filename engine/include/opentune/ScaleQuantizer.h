#pragma once

namespace opentune {

// The musical scale a ScaleQuantizer snaps frequencies onto.
//
// Chromatic is "every semitone allowed" -- the twelve-tone equal-tempered
// grid with no notes excluded, which is what makes it the only scale that
// needs no key/root information to define: any input frequency has a
// nearest semitone regardless of what key the song is in. The other
// members name scales to be added in a later stage (see ScaleQuantizer.cpp
// for the Stage 5 placeholder note); until then, every ScaleType value
// behaves identically to Chromatic.
enum class ScaleType { Chromatic, Major, NaturalMinor, HarmonicMinor, Pentatonic };

// A scale is a type (which scale-degree pattern) plus a root. `rootMidiNote`
// names the tonic as a MIDI note number (e.g. 60 = C4) that the pattern in
// `type` is built from -- e.g. {60, Major} is C major. Chromatic has no
// scale-degree pattern to anchor, so `rootMidiNote` is unused when
// `type == ScaleType::Chromatic`; it exists on the struct because every
// other ScaleType needs it.
struct Scale {
    int rootMidiNote;
    ScaleType type;
};

// Snaps a detected frequency to the nearest pitch allowed by its Scale --
// the piece of the engine that turns "whatever frequency the singer
// produced" into "the frequency they were reaching for." See specs.md
// section 7.1 for the exact contract.
//
// It is a pure function of its input frequency and the currently-set
// Scale: no history, no internal buffers, nothing time-dependent. That
// makes it trivially real-time safe (constitution II) and trivially
// testable -- given the same frequency and scale, snap() always returns
// the same answer.
//
// Stage 0 (this task) implements only chromatic quantization: every
// ScaleType value currently snaps to the nearest equal-tempered semitone,
// regardless of key. Real scale tables (Major, NaturalMinor,
// HarmonicMinor, Pentatonic -- restricting which semitones are allowed,
// based on `rootMidiNote`) are Stage 5 work; see ScaleQuantizer.cpp.
class ScaleQuantizer {
public:
    explicit ScaleQuantizer(Scale scale) noexcept;

    // Returns the frequency, in Hz, of the nearest allowed pitch to
    // `frequencyHz`. Real-time safe: called once per voiced audio block on
    // the audio thread, so it never allocates, throws, locks, logs, or
    // blocks (constitution II), hence noexcept.
    float snap(float frequencyHz) const noexcept;

    // Replaces the scale used by subsequent snap() calls. Real-time safe
    // (constitution II): a Scale is two plain scalars, so this is a cheap
    // in-place assignment, not a reallocation -- safe to call from the
    // audio thread, e.g. when the user changes key mid-performance.
    void setScale(Scale scale) noexcept;

private:
    Scale m_scale;
};

} // namespace opentune
