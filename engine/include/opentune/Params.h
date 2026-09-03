#pragma once

#include "opentune/ScaleQuantizer.h"

namespace opentune {

// The engine's entire user-facing control surface: every dial a host (CLI
// flag, GUI slider, Android control) exposes to shape how correction sounds.
// See specs.md section 7.1 for the exact contract -- fields, order, and
// defaults below are copied from it verbatim.
//
// Params is a plain data struct on purpose (constitution: no logic on the
// audio path beyond what is strictly necessary). A host builds one, mutates
// it in response to user input, and hands it to the engine; the engine reads
// it once per block on the audio thread. Because that handoff crosses from a
// UI/control thread onto the real-time audio thread, Params must be cheap
// and safe to copy by value with no synchronization beyond the handoff
// mechanism itself (e.g. a lock-free atomic swap, per specs.md section
// 7.1's Engine.h note) -- see the static_assert in tests/test_params.cpp,
// which pins Params as trivially copyable for exactly this reason.
struct Params {
    // The single dial spanning transparent correction to hard-tune (see
    // specs.md G2 -- this is the product's core idea). Range 0.0-1.0:
    //   0.0 = bypass. The detected pitch is not pulled toward the scale at
    //        all; output pitch equals input pitch. Correction is inaudible
    //        because there is none.
    //   1.0 = full snap. Every voiced frame is pulled all the way to its
    //        nearest scale pitch, as fast as retuneMs allows. This is the
    //        "hard-tune" / "T-Pain" effect: robotic, quantized-sounding
    //        pitch with audible stair-steps between notes.
    //   Values in between blend the two: the output pitch moves only
    //        `strength` of the way from the detected pitch to the target
    //        pitch, so correction nudges a singer toward the scale without
    //        erasing their natural pitch drift and vibrato. Default 0.8
    //        leans heavily corrected while still leaving a little of the
    //        original performance audible.
    float strength = 0.8f;

    // How fast, in milliseconds, corrected pitch glides to its target once
    // a new target is chosen. Conceptually a time-constant on a smoothing
    // filter applied to the pitch-correction amount, not a hard delay.
    //   Small values (single-digit ms) = the pitch snaps to the target
    //        almost instantly. This produces the robotic, stepped "hard
    //        tune" character -- there is no audible transition between
    //        notes, just a jump.
    //   Large values (tens to hundreds of ms) = the pitch glides to the
    //        target gradually, giving correction time to be heard rather
    //        than instantaneous. This preserves expressive pitch movement
    //        that happens faster than the glide -- scoops into a note and
    //        vibrato -- because the corrector doesn't have time to fully
    //        chase them before they're already moving again (see FR8;
    //        specs.md's listening pass requires vibrato to survive at
    //        retuneMs >= 60). Default 40 ms is fast enough to feel
    //        responsive but slow enough not to sound purely mechanical.
    float retuneMs = 40.0f;

    // Deliberate residual pitch deviation, in cents, that correction does
    // not remove even when strength pulls the note fully to target. A
    // perfectly flat, dead-on-pitch tone with zero deviation reads to the
    // ear as artificial -- real (even excellent) vocal performances always
    // wobble a little. humanizeCents lets a small amount of that wobble
    // through on purpose so hard-tuned output doesn't sound sterile or
    // synthetic. Range is 0 and up (0 = no deliberate deviation retained,
    // i.e. as clean/robotic as `strength` and `retuneMs` alone would
    // produce); default 15 cents is small -- well under a semitone (100
    // cents) -- so it reads as "alive" rather than "out of tune."
    float humanizeCents = 15.0f;

    // Whether pitch correction preserves the singer's vocal-tract formants
    // (the resonant frequency bands that give a voice its timbre and,
    // combined with pitch, make it sound like a particular person). Naively
    // shifting pitch by resampling shifts formants by the same ratio, which
    // is what produces the "chipmunk" artifact: raise a voice's pitch that
    // way and its formants rise with it, so the singer starts to sound like
    // a smaller person (or a cartoon chipmunk) rather than the same person
    // singing higher. Enabling this (the default) keeps formants fixed
    // while pitch moves, so corrected output still sounds like the
    // original singer's voice (FR9). Disabling it is mainly useful for
    // deliberately chasing that chipmunk/robotic character as an effect.
    bool preserveFormants = true;

    // Which notes are legal correction targets. A Scale is a root note plus
    // a pattern (ScaleType) -- see ScaleQuantizer.h -- and the
    // ScaleQuantizer snaps every detected pitch to the nearest frequency
    // that scale allows. The default, chromatic with root MIDI note 0,
    // requires no key selection (FR4): every semitone is a legal target,
    // so any input frequency has a nearest one regardless of what key the
    // performance is actually in. Selecting a musical scale (Major,
    // NaturalMinor, ...) with a specific root restricts legal targets to
    // that scale's notes, so an out-of-key sung note snaps to the nearest
    // in-key one instead of merely the nearest semitone (FR3, FR5).
    Scale scale = {0, ScaleType::Chromatic};
};

} // namespace opentune
