#pragma once

namespace opentune {

// Interface every pitch corrector implements: given a block of mono audio and
// a target pitch ratio, it produces pitch-shifted audio. See specs.md section
// 7.1 for the exact contract; implementations include `ResampleCorrector`
// (naive, Stage 0) and `SignalsmithCorrector` (Stage 2, formant-preserving).
//
// Where PitchDetector answers "what pitch is this?", PitchCorrector answers
// "make this a different pitch." The two are independent: a host (Engine)
// measures the input's pitch with a PitchDetector, works out how far that
// pitch is from the nearest scale tone (ScaleQuantizer), and hands the
// resulting ratio to a PitchCorrector to actually shift the audio.
class PitchCorrector {
public:
    virtual ~PitchCorrector() = default;

    // One-time (or parameter-change-time) setup, called before streaming
    // begins. May allocate: this is where any internal buffers a real
    // implementation needs -- e.g. a delay line for time-domain resampling,
    // or analysis/synthesis windows for a phase-vocoder approach -- are
    // sized for the given sample rate and block size and allocated once.
    // Not real-time safe and not noexcept -- never called from the audio
    // thread while it is processing blocks.
    virtual void prepare(double sampleRate, int maxBlockSize) = 0;

    // Clears internal state (e.g. delay line contents, phase accumulators)
    // back to the post-prepare() state, without freeing or reallocating
    // anything. Real-time safe: called on the audio thread, e.g. when
    // transport restarts. noexcept per constitution II.
    virtual void reset() noexcept = 0;

    // Shifts one block of `n` mono samples from `in` to `out` by
    // `pitchRatio`, and always writes exactly `n` output samples.
    //
    // `pitchRatio` is output frequency divided by input frequency: 1.0
    // leaves pitch unchanged, 2.0 raises it an octave, 0.5 lowers it an
    // octave. A ratio of 1.0 is the identity case -- bit-identical
    // passthrough is the ideal, though a real (e.g. resampling or
    // phase-vocoder) implementation may not hit that exactly, since its
    // signal path runs through the same interpolation/windowing machinery
    // regardless of ratio and that machinery is not perfectly transparent
    // at unity. Callers (Engine) compute this ratio from the detected pitch
    // and the nearest scale tone; this interface does not know about scales
    // or detectors at all, only the ratio it is told to apply.
    //
    // `in` and `out` are separate buffers rather than one in-place buffer
    // because most pitch-shifting algorithms cannot produce sample `i` of
    // the output from only sample `i` of the input -- they read a window of
    // input samples (past, and for non-causal designs, future-in-the-window
    // but already-buffered) to produce each output sample, so writing
    // results back over `in` while still reading from it would corrupt
    // input the algorithm has not consumed yet. Separate buffers also let a
    // real-time-safe implementation avoid ever needing a temporary copy: it
    // can read from `in` and write to `out` in a single pass without an
    // internal scratch allocation. `in` and `out` must not overlap.
    //
    // Real-time safe: called once per audio block on the audio thread, so
    // it must never allocate, throw, lock, log, or block, hence noexcept.
    // It never reads `in[i + 1]` while producing `out[i]` beyond what its
    // internal buffering (sized in prepare()) already holds -- no reading
    // of not-yet-arrived samples (no lookahead, constitution III). Before
    // its internal buffers have enough history to shift confidently, it
    // still produces `n` samples of output -- passthrough or silence rather
    // than nothing -- per the block contract in engine/CLAUDE.md.
    virtual void process(const float* in, float* out, int n, float pitchRatio) noexcept = 0;

    // How many samples of delay this implementation introduces between an
    // input sample arriving and the corresponding (possibly shifted) output
    // sample appearing, so a host can compensate -- e.g. to keep a
    // dry/wet mix or a video track in sync.
    //
    // A real-time pitch corrector is causal (constitution III: it never
    // reads future samples), so to shift pitch at all it typically has to
    // accumulate a window of input before it can produce output derived
    // from that window: a phase vocoder needs a full analysis frame, and
    // even simple time-domain resampling needs enough buffered history to
    // interpolate smoothly across block boundaries without clicks. That
    // buffering is exactly what introduces latency -- there is no way to
    // shift pitch on sample `i` using only samples up to `i` with zero
    // delay, because pitch is a property of a waveform over time, not of a
    // single sample.
    //
    // Real-time safe: called on the audio thread (e.g. once at stream
    // start to configure output routing), so it must never allocate,
    // throw, lock, log, or block, hence noexcept. It reports a value fixed
    // by prepare(), not one that changes per block.
    virtual int latencySamples() const noexcept = 0;
};

} // namespace opentune
