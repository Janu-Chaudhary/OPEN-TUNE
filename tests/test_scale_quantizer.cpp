#include "doctest.h"
#include "opentune/ScaleQuantizer.h"

#include <cmath>
#include <utility>

// T0.5 done-criteria (tasks.md): 440 Hz -> 440 Hz (A4 exactly); 445 Hz -> 440 Hz;
// 452 Hz -> 466.16 Hz (A#4); A4 = 440 Hz reference; correct across the full C2-C6
// range. Only ScaleType::Chromatic is exercised here -- it is the only scale
// implemented in this task (see ScaleQuantizer.h / ScaleQuantizer.cpp).

namespace {

// Wraps doctest::Approx with an *absolute* Hz tolerance, per the brief
// ("tolerance of 0.01 Hz"). doctest::Approx's own comparison is
// `|lhs - value| <= epsilon * (scale + max(|lhs|, |value|))`; setting
// scale(0) and epsilon(0.01 / expected) collapses that bound to exactly
// 0.01 Hz around `expected`, regardless of `expected`'s magnitude. Only
// used for expected values that are strictly positive (all musical
// frequencies here are); the frequencyHz <= 0 passthrough case is checked
// with a plain equality instead, since 0.01/0 is undefined.
doctest::Approx approxHz(float expectedHz) {
    return doctest::Approx(static_cast<double>(expectedHz))
        .epsilon(0.01 / static_cast<double>(expectedHz))
        .scale(0.0);
}

} // namespace

TEST_CASE("ScaleQuantizer chromatic: A4 (440 Hz) snaps to itself exactly") {
    opentune::ScaleQuantizer quantizer({0, opentune::ScaleType::Chromatic});
    CHECK(quantizer.snap(440.0f) == approxHz(440.0f));
}

TEST_CASE("ScaleQuantizer chromatic: 445 Hz snaps down to A4 (440 Hz)") {
    // 445 Hz is ~19.6 cents sharp of A4 -- well inside the +/-50 cent
    // capture range of the A4 semitone, so it snaps down to 440 Hz.
    opentune::ScaleQuantizer quantizer({0, opentune::ScaleType::Chromatic});
    CHECK(quantizer.snap(445.0f) == approxHz(440.0f));
}

TEST_CASE("ScaleQuantizer chromatic: 455 Hz snaps up to A#4 (466.16 Hz)") {
    // 455 Hz is closer to A#4 (466.16 Hz, MIDI 70) than to A4 (440 Hz,
    // MIDI 69): it is ~58 cents sharp of A4, more than halfway (50 cents)
    // to the next semitone up.
    //
    // NOTE: the task brief's worked example uses "452 Hz -> 466.16 Hz",
    // but 452 Hz is only ~46.6 cents sharp of A4 -- inside A4's capture
    // range, not A#4's -- per the exact formula this task is required to
    // use (69 + 12*log2(f/440), engine/CLAUDE.md Numerics; boundary works
    // out to ~452.89 Hz). 452 Hz correctly snaps to 440 Hz (see the "49/51
    // cents" boundary test below, which pins this rounding rule exactly).
    // 455 Hz is used here instead so this test's expected value is
    // consistent with the formula. Flagged in the task report.
    opentune::ScaleQuantizer quantizer({0, opentune::ScaleType::Chromatic});
    CHECK(quantizer.snap(455.0f) == approxHz(466.16f));
}

TEST_CASE("ScaleQuantizer chromatic: 49 cents sharp snaps down, 51 cents sharp snaps up") {
    // Exercises the rounding boundary at exactly the midpoint (50 cents)
    // between two adjacent semitones, using A4 (440 Hz) and A#4 as the
    // pair. cents = 1200 * log2(f / 440) (engine/CLAUDE.md Numerics).
    opentune::ScaleQuantizer quantizer({0, opentune::ScaleType::Chromatic});

    const float fortyNineCentsSharp = 440.0f * std::pow(2.0f, 49.0f / 1200.0f);
    const float fiftyOneCentsSharp = 440.0f * std::pow(2.0f, 51.0f / 1200.0f);

    CHECK(quantizer.snap(fortyNineCentsSharp) == approxHz(440.0f));

    const float a4Sharp4 = 440.0f * std::pow(2.0f, 1.0f / 12.0f); // A#4
    CHECK(quantizer.snap(fiftyOneCentsSharp) == approxHz(a4Sharp4));
}

TEST_CASE("ScaleQuantizer chromatic: frequencyHz <= 0 passes through as 0 (unvoiced)") {
    // Per the brief: frequencyHz <= 0 is not a pitch to quantize (it is the
    // PitchEstimate convention for "unvoiced", see PitchDetector.h), so
    // snap() returns 0 rather than producing a nonsensical MIDI note from
    // log2 of a non-positive number.
    opentune::ScaleQuantizer quantizer({0, opentune::ScaleType::Chromatic});
    CHECK(quantizer.snap(0.0f) == 0.0f);
    CHECK(quantizer.snap(-10.0f) == 0.0f);
}

TEST_CASE("ScaleQuantizer chromatic: every MIDI note 36..84 (C2..C6) snaps to itself") {
    opentune::ScaleQuantizer quantizer({0, opentune::ScaleType::Chromatic});

    // MIDI -> Hz: f = 440 * 2^((m-69)/12), A4 = 69 = 440 Hz (engine/CLAUDE.md
    // Numerics). MIDI 36 is C2, MIDI 84 is C6 -- the full range named in the
    // brief.
    for (int midiNote = 36; midiNote <= 84; ++midiNote) {
        const float exactHz =
            440.0f * std::pow(2.0f, (static_cast<float>(midiNote) - 69.0f) / 12.0f);
        CAPTURE(midiNote);
        CHECK(quantizer.snap(exactHz) == approxHz(exactHz));
    }
}

TEST_CASE("ScaleQuantizer chromatic: setScale replaces the stored scale") {
    // setScale() is real-time safe (noexcept) and simply swaps the stored
    // Scale; snap() continues to behave chromatically since Chromatic is
    // the only scale implemented in this task.
    opentune::ScaleQuantizer quantizer({0, opentune::ScaleType::Chromatic});
    quantizer.setScale({0, opentune::ScaleType::Major});
    CHECK(quantizer.snap(440.0f) == approxHz(440.0f));
}

// constitution II / engine/CLAUDE.md: snap() is a real-time function and
// must never allocate, throw, lock, log, or block -- noexcept is the
// compiler-enforced half of that contract.
static_assert(noexcept(std::declval<const opentune::ScaleQuantizer&>().snap(440.0f)),
              "ScaleQuantizer::snap must be noexcept (audio thread, constitution II)");
static_assert(noexcept(std::declval<opentune::ScaleQuantizer&>().setScale(opentune::Scale{
                  0, opentune::ScaleType::Chromatic})),
              "ScaleQuantizer::setScale must be noexcept (audio thread, constitution II)");
