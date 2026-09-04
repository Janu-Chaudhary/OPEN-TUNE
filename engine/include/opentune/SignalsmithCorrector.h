#pragma once

#include "opentune/PitchCorrector.h"

#include <memory>

namespace opentune {

// A real (phase-vocoder) pitch corrector, built on the vendored Signalsmith
// Stretch library (third_party/signalsmith-stretch/, see its VENDORED.md and
// docs/decisions/0005-signalsmith-pulled-into-stage-0.md for why this exists
// and why it was pulled into Stage 0 ahead of schedule).
//
// Where `ResampleCorrector` (T0.7) is a naive resampler -- it reads the
// input faster or slower and lets duration and formants drift with pitch --
// this corrector uses a phase vocoder: it analyses short overlapping windows
// of input into a magnitude/phase spectrum (an FFT per window), moves each
// frequency bin's energy to a new bin (that's the pitch shift), and
// resynthesises overlapping windows of output from the shifted spectrum. The
// key property this buys us: duration is a property of *how many output
// windows you resynthesise*, not of how fast you read the input, so a
// vocoder can shift pitch without also changing tempo -- which is exactly
// the failure mode `ResampleCorrector` cannot avoid (see its header and
// docs/decisions/0005). Signalsmith Stretch additionally does peak-tracking
// phase locking (matching phases across a spectral peak's neighbouring bins,
// not just at the peak bin) to reduce the smeared, "phasy" sound that a
// naive phase vocoder produces -- see the library's README and its ADC22
// talk ("Four Ways To Write A Pitch-Shifter") for the algorithm.
//
// --- Constitution II (the risk this class exists to manage) ----------------
//
// The vendored header uses std::vector, std::function, and std::random.
// Every one of those types can allocate, and process() must never allocate,
// throw, lock, log, or block (constitution II; engine/CLAUDE.md). The way
// this class stays real-time safe: `prepare()` calls the library's
// `configure()` once, which is where *all* of its internal std::vector
// buffers are sized (via resize()/reserve()) for the chosen block/interval
// size and channel count -- see the .cpp for the full audit of what
// configure() allocates and why nothing process() calls can grow those
// buffers afterwards. `process()` itself only calls `setTransposeFactor()`
// (assigns a couple of Sample-typed member fields -- no allocation) and
// `process()` on the library object (which reuses buffers already sized in
// configure(); the .cpp explains the one place -- an internal
// `tmpProcessBuffer.resize()` -- that looked suspicious and why it is safe:
// it only ever shrinks-or-holds within the capacity `configure()` reserved).
//
// A hidden implementation detail (the actual `signalsmith::stretch::
// SignalsmithStretch<float>` object, plus the small IO adapter structs
// process() needs) is kept behind a pointer-to-implementation ("pimpl") so
// that including *this* header does not drag the vendored header -- and the
// relaxed warning treatment it needs (see engine/CMakeLists.txt's SYSTEM
// include for it) -- into every translation unit that merely wants to hold
// a `PitchCorrector*`. Only SignalsmithCorrector.cpp includes the vendored
// header.
//
// --- Latency -----------------------------------------------------------
//
// Unlike `ResampleCorrector` (0 latency by construction: see its header),
// a phase vocoder is inherently non-zero-latency: it cannot emit a shifted
// sample until it has analysed a full window of *future-relative-to-that-
// window's-centre* input, per constitution III's "no lookahead beyond what
// has already arrived" -- the window itself is the buffering that makes
// this legal (nothing in it is unarrived audio; the corrector just hasn't
// finished *analysing* what it already has). `latencySamples()` reports the
// real, measured cost of the configuration `prepare()` chose (see the .cpp
// for the number and why that configuration, not the library's quality-
// tuned default, was chosen for Stage 0).
class SignalsmithCorrector final : public PitchCorrector {
public:
    SignalsmithCorrector();
    ~SignalsmithCorrector() override;

    // Not copyable/movable: m_impl owns a heap object; there is no need for
    // this class to be copied or moved anywhere it is used (held behind a
    // PitchCorrector* / unique_ptr<PitchCorrector>), and deleting these
    // avoids having to think about what a partially-configured Signalsmith
    // Stretch object copied mid-stream would even mean.
    SignalsmithCorrector(const SignalsmithCorrector&) = delete;
    SignalsmithCorrector& operator=(const SignalsmithCorrector&) = delete;
    SignalsmithCorrector(SignalsmithCorrector&&) = delete;
    SignalsmithCorrector& operator=(SignalsmithCorrector&&) = delete;

    // Configures the Signalsmith Stretch object (mono, one channel) for
    // `sampleRate` and allocates all of its internal buffers. See the .cpp
    // for the exact block/interval size chosen and the measured latency
    // that choice costs. May allocate (constitution II permits this in
    // prepare(); never called from the audio thread while streaming).
    //
    // `maxBlockSize` is accepted per the PitchCorrector interface but is
    // not used to size anything here: Signalsmith Stretch's own internal
    // buffering (sized from `sampleRate` alone, in the .cpp) is chosen to
    // comfortably exceed any block size this engine's block contract
    // produces (specs.md section 6: 256 nominal). This is an assumption,
    // not a guarantee the library enforces on our behalf, so prepare()
    // checks it explicitly and throws std::invalid_argument if
    // `maxBlockSize` would ever exceed the library's own per-call capacity
    // (`blockSamples() + intervalSamples()`, per the .cpp's allocation
    // audit of `copyInput()`) -- prepare() is setup code, not real-time
    // (see the class table in engine/CLAUDE.md), so throwing here is
    // allowed and is preferable to silently truncating audio in
    // process() later. See the .cpp for the exact check.
    void prepare(double sampleRate, int maxBlockSize) override;

    // Clears the corrector's internal phase-vocoder state (analysis/
    // synthesis buffers, phase accumulators, the random engine used for
    // phase randomisation at large pitch ratios) back to a fresh, silent
    // state -- without freeing or reallocating anything (constitution II):
    // the vendored library's own reset() reassigns its buffers from
    // already-allocated copies and re-fills already-sized vectors in place.
    // See the .cpp's allocation audit.
    void reset() noexcept override;

    // Shifts `n` samples of mono audio by `pitchRatio` using the phase
    // vocoder described above. Sets the library's transpose factor for
    // this block (a plain field assignment, not a reconfiguration -- see
    // the class comment) and delegates to the library's own process(),
    // which is designed to be called repeatedly with arbitrarily small
    // blocks (it accumulates them internally against its own, larger,
    // analysis window) -- exactly the "process() gets one nominal block at
    // a time, sample rate and pitch ratio move underneath it" pattern
    // FR-relevant callers (Engine) use.
    //
    // Before prepare() has been called (m_impl is null), or for a
    // degenerate n <= 0, writes silence for `n` samples rather than reading
    // through an unconfigured or null object -- the block contract
    // (engine/CLAUDE.md) requires exactly n samples of output regardless.
    void process(const float* in, float* out, int n, float pitchRatio) noexcept override;

    // Returns the real, measured latency (input + output halves, both
    // reported by the library) of the configuration prepare() chose, in
    // samples. Computed once in prepare() and cached; this function itself
    // does no work beyond returning that cached value, so it stays
    // noexcept and O(1) as the interface requires.
    int latencySamples() const noexcept override;

private:
    // Forward-declared in the header, defined in the .cpp: holds the actual
    // `signalsmith::stretch::SignalsmithStretch<float>` object plus the
    // tiny IO-adapter structs process() constructs on the stack each call.
    // Keeping it opaque here is what keeps the vendored header (and its
    // relaxed warnings) out of every consumer of *this* header -- see the
    // class comment above.
    struct Impl;
    std::unique_ptr<Impl> m_impl;

    // Cached result of m_impl->stretch.inputLatency() +
    // m_impl->stretch.outputLatency() as of the last prepare() call. See
    // latencySamples() and the .cpp for the measured value and preset
    // choice.
    int m_latencySamples = 0;
};

} // namespace opentune
