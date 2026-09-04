#include "opentune/SignalsmithCorrector.h"

// The vendored library. This is the ONLY translation unit that includes it
// (see the header's class comment) -- engine/CMakeLists.txt marks its
// include directories SYSTEM so its (many, and none of our business) warnings
// are suppressed without touching our own -Wconversion et al. treatment, and
// without editing the vendored file itself (T0.13 brief: vendored code is
// unmodified).
#include "signalsmith-stretch/signalsmith-stretch.h"

#include <algorithm>
#include <stdexcept>

namespace opentune {

// --- Allocation audit (constitution II) -------------------------------
//
// Read alongside third_party/signalsmith-stretch/signalsmith-stretch.h.
// Every std::vector member the library owns, and where it is sized:
//
//   tmpProcessBuffer, tmpPreRollBuffer, _channelBands, peaks (reserve),
//   energy, smoothedEnergy, outputMap, channelPredictions, formantMetric
//
// -- are ALL sized exclusively inside `configure()` (via `.resize()` /
// `.reserve()`), which this class calls exactly once, from `prepare()`.
// `setTransposeFactor()` -- called every block, since Engine recomputes the
// ratio every block -- only assigns two `Sample` (float) member fields
// (`freqMultiplier`, `freqTonalityLimit`); it touches no container at all.
//
// The one call inside `process()` that looked, at a glance, like it could
// reallocate is `copyInput()`'s
//     tmpProcessBuffer.resize(length)
// (`length = std::min(blockSamples() + defaultInterval(), toIndex -
// prevCopiedInput)`). But `length` is capped by `blockSamples() +
// defaultInterval()`, which is exactly the capacity `configure()` already
// gave `tmpProcessBuffer` -- so this resize() only ever shrinks the vector's
// *size* within its existing *capacity*; std::vector::resize() to n <=
// capacity() never reallocates (it's a guarantee of the standard's
// complexity clause, not an implementation detail we're hoping holds). The
// same reasoning covers `flush()`'s `tmpProcessBuffer.resize(tailSamples)`
// (tailSamples <= one interval, well under capacity) and `seek()`'s
// `tmpProcessBuffer.resize(0); tmpProcessBuffer.resize(...)` (0 never
// reallocates; the regrow targets the same capacity again). `reset()`
// reassigns `stashedInput`/`stashedOutput` from `stft.input`/`stft.output`
// (copy-assignment of already-identically-sized objects: no growth) and
// `_channelBands.assign(_channelBands.size(), Band())` (assign() to the
// container's own current size: no growth).
//
// `findPeaks()` (called from inside process(), every analysed frame) does
// `peaks.resize(0)` then `peaks.emplace_back(...)` in a loop. `peaks` was
// `.reserve(bands / 2)`'d in configure(). The loop structure (skip at least
// one bin between the end of one candidate peak region and the start of the
// next -- see the `++start` after each region closes) bounds the number of
// peaks found to at most `bands / 2`, matching the reservation exactly, so
// this cannot grow past capacity either. This was verified empirically, not
// just read off the source: see the allocation-counting test in
// tests/test_signalsmith_corrector.cpp, which runs 10 seconds of audio
// (many thousands of analysis frames, both pitch directions) through
// process() with a global operator new/delete counter armed, and asserts
// zero heap operations happen after prepare().
//
// `randomEngine` (a `std::default_random_engine`, no allocation itself) is
// seeded once at construction; `std::uniform_real_distribution` objects
// built inside `processSpectrum()` are small stack objects (the standard
// does not permit distribution types to allocate) constructed fresh each
// call -- stack construction, not heap allocation, and therefore not a
// constitution II concern despite being "constructed every call."
//
// Net result: process() does not allocate, for any pitchRatio, including
// the every-block ratio changes this engine actually does. If a future
// library update changes this, the allocation-counting test above will
// catch it (it is not merely asserted, it is demonstrated on every test
// run).

struct SignalsmithCorrector::Impl {
    // channels=1 (mono; specs.md section 6). RandomEngine=void picks the
    // library's default (std::default_random_engine) -- we have no reason
    // to inject a different one.
    signalsmith::stretch::SignalsmithStretch<float> stretch;

    // --- IO adapters -----------------------------------------------------
    // process()'s template Inputs/Outputs concept only requires
    // `buffer[channel]` to yield something itself indexable by sample
    // ([index] -> Sample); see the library's README ("any type where
    // buffer[channel][index] gives you a sample"). For our mono use, both
    // adapters just ignore the channel index and return the one buffer we
    // have. These are trivial, stack-constructed, non-owning wrapper
    // structs -- constructing one is not an allocation.
    struct MonoInput {
        const float* data;
        const float* operator[](int /*channel*/) const noexcept { return data; }
    };
    struct MonoOutput {
        float* data;
        float* operator[](int /*channel*/) const noexcept { return data; }
    };
};

SignalsmithCorrector::SignalsmithCorrector() = default;

// Defined here (not defaulted in the header) because std::unique_ptr's
// destructor needs Impl's complete type, which is only visible in this
// translation unit -- the standard pimpl requirement.
SignalsmithCorrector::~SignalsmithCorrector() = default;

void SignalsmithCorrector::prepare(double sampleRate, int maxBlockSize) {
    m_impl = std::make_unique<Impl>();

    // --- Preset choice, and why -----------------------------------------
    //
    // Signalsmith Stretch ships two presets (block/interval are fractions
    // of sampleRate; `splitComputation` is each preset's own default, NOT
    // "off" for both -- getting this backwards is exactly the bug fix round
    // 1 caught here, see below):
    //   presetDefault(channels, sampleRate)
    //       -> block = 0.12 * sampleRate, interval = 0.03 * sampleRate,
    //          splitComputation defaults to FALSE
    //   presetCheaper(channels, sampleRate)
    //       -> block = 0.10 * sampleRate, interval = 0.04 * sampleRate,
    //          splitComputation defaults to TRUE
    // Both are tuned by the library's author for quality/CPU on typical
    // material, not for a latency budget. AC4 (specs.md) caps the *whole*
    // pipeline at 20 ms, and T2.5/T3.8 will hold us to it once this runs
    // live. Stage 0 is offline (a WAV in, a WAV out -- no live monitoring
    // yet), so nothing breaks today either way, but accepting a
    // higher-latency configuration here would be "solving" Stage 0's
    // problem by quietly creating Stage 3's -- exactly what
    // docs/decisions/0005 warns against ("the preset choice is a real
    // decision, not a default to accept").
    //
    // `configure()`'s block/interval sizes are plain integer sample counts,
    // not tied to either named preset, so a future task tuning for a real
    // latency budget (T2.5/T3.8) has a continuous knob available rather
    // than being stuck choosing between only these two presets. That
    // tuning is out of this task's scope -- Stage 0 only needs the
    // checkpoint to be audible, not real-time-tight -- so it is left for
    // T2.5/T3.8 to do deliberately, against a measured budget, rather than
    // guessed at here.
    //
    // `presetCheaper` is named for being cheaper in CPU (a larger hop means
    // fewer FFTs per second), NOT for latency: its own default
    // `splitComputation = true` adds one whole extra interval (1920
    // samples at 48 kHz, 40 ms) of output latency to smooth CPU spikes
    // across a live audio thread -- a real, valuable trade for Stage 3's
    // real-time host, but pure cost with no offsetting benefit for this
    // offline, one-shot WAV-in/WAV-out run, which has no audio thread to
    // protect from spikes. So we call `presetCheaper(1, sampleRate,
    // false)`, explicitly overriding that default: block = 0.10 *
    // sampleRate, interval = 0.04 * sampleRate, no split-computation
    // latency tax. At 48 kHz this measures out to `latencySamples() ==
    // blockSamples() == 4800` (100 ms) -- genuinely below `presetDefault`'s
    // 120 ms, which is the actual basis for preferring this preset (not
    // its name). Recorded in the T0.13/T0.14 report; still expected to
    // exceed the 20 ms AC4 budget, and that gap is exactly what T2.5
    // (latency accounting) and T3.8 (verify <= 20 ms, likely via a smaller
    // custom `.configure()` per the paragraph above) exist to close.
    m_impl->stretch.presetCheaper(1, static_cast<float>(sampleRate), false);

    // --- maxBlockSize guard ----------------------------------------------
    //
    // process() hands whatever it's given straight to the library's own
    // process(), which internally copies input in chunks no larger than
    // `blockSamples() + intervalSamples()` per call (see the allocation
    // audit's discussion of `copyInput()` -- that sum is exactly the
    // capacity its internal `tmpProcessBuffer` was reserved to). If a
    // caller ever handed us a single block larger than that, the "no
    // reallocation" property this whole file is about would no longer
    // hold for that call: `resize()` would be asked to grow past capacity,
    // which is exactly the constitution II breach this class exists to
    // avoid. specs.md section 6 says block size is 256 nominal, so this
    // never binds in practice (256 is a small fraction of the 4800 + 1920
    // = 6720-sample capacity presetCheaper(false) gives us at 48 kHz), but
    // "never happens under the documented contract" is not the same as
    // "cannot happen," so it is checked here rather than merely assumed.
    // This is setup code (constructors/prepare(), per engine/CLAUDE.md's
    // table), so throwing is allowed -- unlike process(), which never
    // reads this value at all.
    const int perCallCapacity =
        static_cast<int>(m_impl->stretch.blockSamples() + m_impl->stretch.intervalSamples());
    if (maxBlockSize > perCallCapacity) {
        throw std::invalid_argument(
            "SignalsmithCorrector::prepare: maxBlockSize exceeds the configured "
            "per-call capacity (blockSamples() + intervalSamples()); process() "
            "would ask the library to grow a buffer it sized once in configure(), "
            "which constitution II forbids");
    }

    // Initial transpose factor: 1.0 (no shift) until the first process()
    // call sets a real one. Configured here, not left implicit, so
    // prepare() fully establishes a well-defined, ready-to-stream state
    // per the PitchCorrector interface contract.
    m_impl->stretch.setTransposeFactor(1.0f);

    m_impl->stretch.reset();

    // Latency is ambiguous for a time/pitch stretcher (the library reports
    // it in two halves -- see its README's "Latency" section): input
    // samples should arrive slightly ahead of the moment they affect the
    // output, and output samples appear slightly behind that same moment.
    // For a host that just wants "how many samples of delay do I need to
    // compensate," the sum of both halves is the right number: it is the
    // total distance, in samples, from an input sample arriving to its
    // effect appearing at the output. Measured (queried from the library,
    // not computed from the block/interval numbers by hand) so this number
    // is guaranteed to track whatever presetCheaper() actually configured.
    m_latencySamples = m_impl->stretch.inputLatency() + m_impl->stretch.outputLatency();
}

void SignalsmithCorrector::reset() noexcept {
    if (m_impl) {
        m_impl->stretch.reset();
    }
}

void SignalsmithCorrector::process(const float* in, float* out, int n, float pitchRatio) noexcept {
    if (n <= 0) {
        return;
    }

    if (!m_impl) {
        // prepare() was never called: nothing is configured to shift
        // anything, so -- per the block contract (engine/CLAUDE.md) --
        // still emit exactly n samples rather than nothing. Silence, not
        // an attempt at passthrough, because there is no dry buffer we
        // could route through safely and no state to reason about yet.
        std::fill(out, out + n, 0.0f);
        return;
    }

    // A plain field assignment inside the library (see the allocation
    // audit above) -- not a reconfiguration, so calling this every block
    // (Engine recomputes the ratio every block; docs/decisions/0005) is
    // exactly the intended usage, not a hidden cost.
    m_impl->stretch.setTransposeFactor(pitchRatio);

    const Impl::MonoInput input{in};
    const Impl::MonoOutput output{out};

    // Equal input/output length: we want a pure pitch shift, not a time
    // stretch (the library supports both -- see its README's "Time-
    // stretching" section -- by taking differently-sized input/output
    // buffers; we deliberately never do that here since duration must not
    // change, which is the entire property ResampleCorrector cannot
    // deliver and this class exists to fix).
    m_impl->stretch.process(input, n, output, n);
}

int SignalsmithCorrector::latencySamples() const noexcept {
    return m_latencySamples;
}

} // namespace opentune
