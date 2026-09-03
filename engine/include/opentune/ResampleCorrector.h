#pragma once

#include "opentune/PitchCorrector.h"

#include <cstdint>
#include <vector>

namespace opentune {

// The naive, textbook pitch corrector: read the input at `pitchRatio` times
// its normal rate, linearly interpolating between samples to fill in the
// fractional positions that fall between two real input samples.
//
// *** THIS IS DELIBERATELY THE WRONG ALGORITHM. IT IS NOT MEANT TO BE FIXED. ***
//
// What it gets wrong, on purpose:
//   1. It changes duration as well as pitch. Reading twice as fast (ratio
//      2.0) does not just double the frequency of everything in the
//      signal -- it also plays through twice as much source material per
//      unit of output time. A real-time, causal API (this one: `process()`
//      always emits exactly `n` samples for `n` samples in, per
//      engine/CLAUDE.md's block contract) cannot summon source material
//      that has not arrived yet, so once the read position "catches up" to
//      the newest sample it has actually been given, it holds (repeats)
//      that sample rather than advancing further -- see process() for
//      exactly when. This is why this implementation is only reliably
//      accurate near the *start* of a stream at ratio > 1.0: read the class
//      comment in tests/test_resample_corrector.cpp for the arithmetic.
//      A correct algorithm (Stage 2, T2.2, Signalsmith Stretch) does not
//      resample at all -- it reconstructs the signal at a new pitch using
//      the *same* amount of source material per unit time, which is why it
//      does not have this failure mode.
//   2. It moves the formants (the vowel-shaping resonances of a voice)
//      along with the pitch, because linear-interpolation resampling
//      literally stretches or compresses the whole waveform, resonances
//      included. That is the "chipmunk" effect: raise the pitch this way
//      and the voice does not just get higher, it sounds smaller/younger,
//      because its formants moved up too. A real voice's formants stay
//      roughly put when *it* sings a higher note; this corrector cannot
//      tell the difference between "pitch" and "everything," because it
//      has no model of either -- it is just resampling.
//
// Both of these are shipped on purpose. Per this task's brief
// (.superpowers/sdd/tasks/task-T0.7-brief.md): shipping the wrong algorithm
// here makes the Stage 0 pipeline audible today, and makes the problem
// Stage 2's `SignalsmithCorrector` solves obvious by ear rather than only
// on paper. Do not add overlap-add, a phase vocoder, or formant correction
// here -- that is exactly the scope of T2.2, and adding any of it in this
// class would defeat the point of this task.
//
// Implementation, briefly (see .cpp for the details and the exact
// read-position arithmetic): a fractional read position advances by
// `pitchRatio` for every output sample produced, persisting across
// `process()` calls so the position drifts continuously rather than
// resetting each block. Each output sample is linearly interpolated
// between the two input samples straddling that position. Because the
// read position can point at an input sample from an earlier `process()`
// call (this happens whenever `pitchRatio < 1.0`, where the read position
// falls further and further behind the samples arriving), a bounded
// history buffer of previously-seen samples is kept so recent history
// remains available -- sized once in prepare(), never resized. It does
// NOT solve unbounded drift: sustained use at pitchRatio far from 1.0 for
// long enough will eventually run past even that buffer, at which point
// the oldest sample still held is used (another documented "hold").
//
// Chunking dependence (documented, not fixed): for `pitchRatio <= 1.0`,
// the read position never asks for a sample beyond what has already
// arrived, so the result does not depend on how the caller splits a given
// input stream into blocks. For `pitchRatio > 1.0` it does: a single large
// `process()` call hands over many already-known "future" samples at once
// (this is fine -- they are not lookahead into audio that has not been
// captured yet, just samples already sitting in the caller's buffer), so a
// big block lets the read position advance further before holding than
// the same total input split into several smaller calls would. A correct
// implementation would not have this quirk; this one does, and it is left
// as-is rather than papered over, consistent with "do not fix the naive
// algorithm."
class ResampleCorrector final : public PitchCorrector {
public:
    // Sizes and allocates the internal history buffer for the given
    // maximum block size. May allocate (constitution II) -- never called
    // from the audio thread while streaming. See .cpp for how the history
    // buffer's size is chosen.
    void prepare(double sampleRate, int maxBlockSize) override;

    // Resets the read position and clears the history buffer's contents
    // back to the post-prepare() state (all zero), without freeing or
    // reallocating anything. Real-time safe (noexcept).
    void reset() noexcept override;

    // Produces `n` output samples by reading `in` (plus whatever history
    // this instance is still holding from earlier calls) at `pitchRatio`
    // times the normal rate, with linear interpolation between samples.
    // Always writes exactly `n` samples, per engine/CLAUDE.md's block
    // contract -- including once the read position has outrun the input
    // this instance has actually been given, in which case it holds
    // (repeats) the most recently available sample rather than emitting
    // silence or nothing; see the .cpp for exactly where that happens and
    // why holding, not silence, was chosen (silence would introduce a
    // sharp discontinuity in the middle of a still-sounding tone, which is
    // no more "correct" and is harder to reason about than a held level).
    //
    // At `pitchRatio == 1.0`, this is bit-identical passthrough (exact
    // float equality, not merely close): the read position advances by
    // exactly 1.0 double-precision unit per output sample, staying an
    // exact integer forever, so the fractional interpolation weight is
    // always exactly 0.0 and this implementation special-cases that weight
    // to copy the input sample directly rather than route it through the
    // interpolation formula (`a*(1-frac) + b*frac`), which is not
    // guaranteed bit-exact under all compilers/optimisation settings even
    // when frac is exactly zero. See .cpp.
    //
    // Real-time safe: never allocates, throws, locks, logs, or blocks
    // (noexcept). Never reads a sample that has not been handed to this
    // instance in this or an earlier process() call (no lookahead,
    // constitution III).
    void process(const float* in, float* out, int n, float pitchRatio) noexcept override;

    // This implementation introduces no additional buffering delay: every
    // output sample is produced from an input sample already delivered in
    // this or an earlier process() call (or, once exhausted, a held copy
    // of one), never from a sample deliberately withheld to build up a
    // fixed-size lookahead window the way e.g. a phase-vocoder's analysis
    // frame would. The pitch-related timing change this corrector produces
    // comes from the read position drifting relative to the write
    // position, not from a constant end-to-end delay a host could usefully
    // compensate for -- so 0 is both the accurate and the only sensible
    // answer here.
    int latencySamples() const noexcept override;

private:
    // How many maxBlockSize's worth of past input to retain for lookback,
    // as a multiple of maxBlockSize (see prepare()). This bounds how far
    // behind the write position the read position can fall (relevant when
    // pitchRatio < 1.0) before this implementation starts holding the
    // oldest sample it still has, per the "chunking dependence" and
    // "history buffer" notes in the class comment above. 8 is a small,
    // arbitrary multiple -- generous enough for every test in
    // tests/test_resample_corrector.cpp to complete without ever hitting
    // that wall, while remaining a small, fixed amount of memory rather
    // than an unbounded one.
    static constexpr int kHistoryBlocks = 8;

    // Sample rate this instance was prepared with. Currently unused by the
    // resampling arithmetic itself (pitchRatio, not sampleRate, drives how
    // fast the read position advances -- see .cpp), but stored because a
    // future revision (or a subclass) may need it, and because storing it
    // costs nothing and documents what prepare() was called with.
    double m_sampleRate = 0.0;

    // Capacity of m_history, in samples: kHistoryBlocks * maxBlockSize,
    // computed once in prepare() and never resized (constitution II).
    int m_historyCapacity = 0;

    // Ring of the most recent m_historyCapacity input samples received
    // *before* the block currently being processed (i.e. NOT including
    // that block itself -- during process(), the current block's own
    // samples are read directly from its `in` pointer, not from this
    // buffer). Implemented as "shift left, append at the tail" -- see
    // AutocorrelationDetector.cpp for the same pattern and why it is still
    // real-time safe: both copies are bounded by m_historyCapacity, a
    // constant fixed at prepare() time.
    std::vector<float> m_history;

    // Total number of input samples this instance has ever been handed,
    // across every process() call since the last prepare()/reset(). Used
    // as the index, into an implied "one long stream of every sample ever
    // received," of the first sample in the block currently being
    // processed. A 64-bit count so it cannot practically overflow even
    // across a very long streaming session (48 kHz for ~6 million years).
    std::int64_t m_samplesReceived = 0;

    // The read position, in the same "one long stream" index space as
    // m_samplesReceived, of the *next* output sample this instance has not
    // yet produced. Persists across process() calls (it is not reset each
    // block) -- that persistence is what makes pitchRatio != 1.0 actually
    // compress or stretch the signal across block boundaries rather than
    // restarting from scratch every call. Advances by pitchRatio for every
    // output sample process() produces.
    double m_readPos = 0.0;
};

} // namespace opentune
