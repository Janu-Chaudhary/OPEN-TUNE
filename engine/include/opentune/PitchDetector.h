#pragma once

namespace opentune {

// A single frame's worth of pitch information, produced by a PitchDetector.
//
// "Voiced" is a term from speech/vocal acoustics: a sound is voiced when it
// carries periodic energy from the vocal folds vibrating -- vowels, hums,
// sung notes. It is unvoiced when there is no such periodicity: silence,
// breath noise, and most consonants (s, t, k, f, ...) are unvoiced. Only
// voiced audio has a meaningful "pitch" to correct; an unvoiced frame has no
// fundamental frequency to report, which is why frequencyHz is 0 in that
// case -- 0 Hz is not a guess at the pitch, it is the absence of one.
struct PitchEstimate {
    // The detected fundamental frequency, in Hz. Meaningful only when
    // `voiced` is true. Always 0.0f when `voiced` is false: there is no
    // periodic signal to measure a frequency from, so reporting anything
    // else would imply a pitch that does not exist.
    float frequencyHz;

    // How confident the detector is in `frequencyHz`, from 0.0 (no
    // confidence) to 1.0 (certain). Detectors use this to signal a
    // borderline read -- e.g. a period estimate found on a frame with weak
    // or ambiguous periodicity -- without silently upgrading it to a firm
    // voiced/unvoiced decision.
    float confidence;

    // True when this frame contains periodic (voiced) energy worth
    // correcting; false for silence, breath, and unpitched consonants. A
    // pitch corrector downstream uses this to decide whether to touch the
    // signal at all (constitution: unvoiced audio must pass through
    // uncorrected).
    bool voiced;
};

// Interface every pitch detector implements: given a block of mono audio, it
// estimates the fundamental frequency of the voice singing or speaking into
// it. See specs.md section 7.1 for the exact contract; implementations include
// AutocorrelationDetector (naive, Stage 0) and YinDetector (Stage 1).
//
// A detector typically receives short blocks (e.g. 256 samples) one at a
// time but needs a longer window (e.g. ~2048 samples) to estimate a low
// fundamental reliably -- one period of a 65 Hz voice at 48 kHz is already
// ~738 samples long, longer than a single block. So a real implementation
// keeps an internal ring buffer of recent samples and analyses that window
// on each call, returning the most recent estimate available. That ring
// buffer is sized once and allocated in `prepare()`, never resized, which is
// why `prepare()` is allowed to allocate while `process()` (called once per
// audio block, on the real-time audio thread) never may: allocation has
// unpredictable cost and can block on the system allocator's internal lock,
// either of which can blow an audio callback's deadline and produce an
// audible dropout. `prepare()` runs once, off the audio thread, before
// streaming starts, where that cost is harmless.
class PitchDetector {
public:
    virtual ~PitchDetector() = default;

    // One-time (or parameter-change-time) setup, called before streaming
    // begins. May allocate: this is where any internal analysis buffers are
    // sized for the given sample rate and block size and allocated once.
    // Not real-time safe and not noexcept -- never called from the audio
    // thread while it is processing blocks.
    virtual void prepare(double sampleRate, int maxBlockSize) = 0;

    // Clears internal state (e.g. ring buffer contents, running estimates)
    // back to the post-prepare() state, without freeing or reallocating
    // anything. Real-time safe: called on the audio thread, e.g. when
    // transport restarts. noexcept per constitution II.
    virtual void reset() noexcept = 0;

    // Analyses one block of `n` mono samples and returns the current pitch
    // estimate. Real-time safe: called once per audio block on the audio
    // thread, so it must never allocate, throw, lock, log, or block, hence
    // noexcept. It never looks past sample `block[n - 1]` (no lookahead,
    // constitution III) and always returns a result immediately, even
    // before enough samples have accumulated to analyse a full window -- in
    // that case it returns `voiced = false` rather than blocking for more
    // input.
    virtual PitchEstimate process(const float* block, int n) noexcept = 0;
};

} // namespace opentune
