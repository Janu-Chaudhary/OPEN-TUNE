// Tests for opentune::Engine (T0.9, fix round 1).
//
// Engine wires PitchDetector -> ScaleQuantizer -> PitchCorrector together
// (specs.md section 7, section 7.1): detect -> quantize -> compute ratio ->
// clamp -> correct.
//
// A note on FR15 (see docs/decisions/0004 for the full corrected history):
// the Stage 0 ScaleQuantizer's nearest-semitone rounding mathematically
// bounds `snap(f)/f` to within +/-50 cents for any positive f, so the
// natural detect -> quantize -> ratio pipeline cannot currently produce a
// ratio anywhere near FR15's clamp bound, regardless of how "wrong" a
// detected frequency is. The clamp itself lives in Engine.cpp as an
// implementation detail (not part of Engine's public surface) and is
// exercised below indirectly, via a PitchCorrector test double that
// records the ratio Engine::process() actually sent it -- proving the
// ratio computed by the real pipeline is delivered to the corrector
// unaltered (clamp is a documented no-op today), not that the clamp can be
// made to visibly engage.
#include "doctest.h"
#include "opentune/AutocorrelationDetector.h"
#include "opentune/Engine.h"
#include "opentune/Params.h"
#include "opentune/PitchCorrector.h"
#include "opentune/PitchDetector.h"
#include "opentune/ResampleCorrector.h"
#include "opentune/ScaleQuantizer.h"

#include "support/Signals.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>
#include <vector>

namespace {

constexpr double kSampleRate = 48000.0;

double centsError(double measuredHz, double referenceHz) {
    return 1200.0 * std::log2(measuredHz / referenceHz);
}

// Same coarse-but-simple zero-crossing frequency estimator used in
// tests/test_resample_corrector.cpp -- see that file for the rationale.
double zeroCrossingFrequency(const std::vector<float>& signal, double sampleRate) {
    int crossings = 0;
    for (std::size_t i = 1; i < signal.size(); ++i) {
        if (signal[i - 1] < 0.0f && signal[i] >= 0.0f) {
            ++crossings;
        }
    }
    const double durationSeconds = static_cast<double>(signal.size()) / sampleRate;
    return static_cast<double>(crossings) / durationSeconds;
}

// True when every sample in signal[blockStart, blockStart + blockSize) is
// bit-identical to the first. This is the shape ResampleCorrector's
// documented "hold the last available sample" degeneration takes once its
// read position has outrun (ratio > 1.0) or fallen behind (ratio < 1.0,
// exhausting its bounded history) the input it has actually been given --
// see tasks.md D5 and finding 4 of task-T0.9-findings-r1.md.
bool isConstantBlock(const std::vector<float>& signal, std::size_t blockStart,
                     std::size_t blockSize) {
    for (std::size_t i = blockStart; i < blockStart + blockSize; ++i) {
        if (signal[i] != signal[blockStart]) {
            return false;
        }
    }
    return true;
}

// A detector that ignores its input entirely and always reports a fixed
// estimate, regardless of how many samples it has seen or how the caller
// chunks them into blocks. Used to isolate a test to Engine's own
// per-block wiring (quantize -> ratio -> correct) from AutocorrelationDetector's
// own block-size-independent-but-buffer-filling behaviour, which is
// already covered by tests/test_autocorrelation_detector.cpp.
class StubPitchDetector final : public opentune::PitchDetector {
public:
    explicit StubPitchDetector(opentune::PitchEstimate estimate) : m_estimate(estimate) {}

    void prepare(double /*sampleRate*/, int /*maxBlockSize*/) override {}
    void reset() noexcept override {}

    opentune::PitchEstimate process(const float* /*block*/, int /*n*/) noexcept override {
        return m_estimate;
    }

private:
    opentune::PitchEstimate m_estimate;
};

// A pass-through corrector that records the last `pitchRatio` and block
// size it was given, and how many times process() was called. Used to
// observe -- "in situ", per finding 6 of task-T0.9-findings-r1.md -- what
// ratio Engine::process() actually hands the corrector, without needing to
// expose Engine's internal clamp function as part of its public API.
class StubPitchCorrector final : public opentune::PitchCorrector {
public:
    explicit StubPitchCorrector(int latencySamples = 0) : m_latencySamples(latencySamples) {}

    void prepare(double /*sampleRate*/, int /*maxBlockSize*/) override {}
    void reset() noexcept override {
        callCount = 0;
        lastRatio = 0.0f;
        lastN = 0;
    }

    void process(const float* in, float* out, int n, float pitchRatio) noexcept override {
        for (int i = 0; i < n; ++i) {
            out[static_cast<std::size_t>(i)] = in[static_cast<std::size_t>(i)];
        }
        lastRatio = pitchRatio;
        lastN = n;
        ++callCount;
    }

    int latencySamples() const noexcept override { return m_latencySamples; }

    int callCount = 0;
    float lastRatio = 0.0f;
    int lastN = 0;

private:
    int m_latencySamples;
};

} // namespace

TEST_CASE("Engine: a 445 Hz sine in is measurably corrected toward 440 Hz, "
          "unlike the uncorrected input") {
    opentune::AutocorrelationDetector detector;
    opentune::ScaleQuantizer quantizer({0, opentune::ScaleType::Chromatic});
    opentune::ResampleCorrector corrector;

    opentune::Params params;
    params.scale = {0, opentune::ScaleType::Chromatic};

    opentune::Engine engine(detector, quantizer, corrector, params);

    constexpr int kMaxBlockSize = 256;
    engine.prepare(kSampleRate, kMaxBlockSize);
    engine.reset();

    // Long enough to run well past AutocorrelationDetector's warm-up window
    // (~2200 samples at 48 kHz) and leave a full-second steady-state tail
    // to measure a frequency from -- but comfortably short of the ~4.84 s
    // (roughly block 907) at which this exact scenario's ratio (0.9888,
    // 445 Hz corrected toward 440 Hz) is known to make ResampleCorrector's
    // bounded history exhaust and the output collapse into a block-rate
    // staircase (tasks.md D5; finding 4 of task-T0.9-findings-r1.md).
    // 150000 samples is 3.125 s, leaving ~1.7 s of margin before collapse.
    constexpr int kTotalSamples = 150000;
    const std::vector<float> in = opentune::test::sine(445.0f, kSampleRate, kTotalSamples);
    std::vector<float> out(static_cast<std::size_t>(kTotalSamples), 0.0f);

    int offset = 0;
    while (offset < kTotalSamples) {
        const int n = std::min(kMaxBlockSize, kTotalSamples - offset);
        engine.process(in.data() + offset, out.data() + offset, n);
        offset += n;
    }

    // Measure only a 1.25 s tail, well after the detector's ring buffer has
    // filled and correction has settled to a steady ratio. A 1.25 s window
    // gives the zero-crossing estimator ~0.8 Hz resolution (~3-4 cents at
    // 440 Hz) -- comfortably tighter than the +/-20 cent tolerance below,
    // unlike the original (0.125 s / 8 Hz / ~31 cent resolution) window,
    // which could not distinguish corrected output from the untouched
    // input (finding 1 of task-T0.9-findings-r1.md).
    constexpr int kMeasureStart = 90000;
    const std::vector<float> measuredOut(out.begin() + kMeasureStart, out.end());
    const std::vector<float> measuredIn(in.begin() + kMeasureStart, in.end());

    const double outHz = zeroCrossingFrequency(measuredOut, kSampleRate);
    const double inHz = zeroCrossingFrequency(measuredIn, kSampleRate);
    const double outCents = centsError(outHz, 440.0);
    const double inCents = centsError(inHz, 440.0);
    CAPTURE(outHz);
    CAPTURE(inHz);
    CAPTURE(outCents);
    CAPTURE(inCents);

    // The corrected output must land close to 440 Hz...
    CHECK(std::abs(outCents) <= 20.0);
    // ...and -- the control assertion finding 1 asks for -- it must land
    // measurably closer to 440 Hz than the same window of the *uncorrected*
    // input signal does. The input is untouched 445 Hz (~19.6 cents sharp
    // of 440); if Engine were not actually correcting anything (e.g. the
    // corrector call were replaced with a passthrough copy), outCents would
    // equal inCents and this assertion -- not just the tolerance above --
    // would fail.
    CHECK(std::abs(outCents) < std::abs(inCents));
}

TEST_CASE("Engine: silence in produces bit-identical silence out") {
    opentune::AutocorrelationDetector detector;
    opentune::ScaleQuantizer quantizer({0, opentune::ScaleType::Chromatic});
    opentune::ResampleCorrector corrector;
    opentune::Params params;

    opentune::Engine engine(detector, quantizer, corrector, params);

    constexpr int kMaxBlockSize = 256;
    engine.prepare(kSampleRate, kMaxBlockSize);
    engine.reset();

    constexpr int kBlockSamples = 256;
    const std::vector<float> in = opentune::test::silence(kBlockSamples);
    // Poisoned with a distinctive non-zero value so a broken bypass (e.g.
    // writing nothing) is caught, not masked by a lucky zero-initialised
    // buffer.
    std::vector<float> out(static_cast<std::size_t>(kBlockSamples), -7.5f);

    engine.process(in.data(), out.data(), kBlockSamples);

    // NOTE: this case alone does not prove FR2 -- ResampleCorrector also
    // returns all zeros for all-zero input at any ratio (see
    // tests/test_resample_corrector.cpp), so this assertion would hold even
    // with the unvoiced bypass deleted. It is kept as the plain "silence
    // in, silence out" sanity check engine/CLAUDE.md's "three tests
    // minimum" asks for; the *unvoiced bypass* itself (FR2) is proven by
    // the white-noise test below, which uses a non-zero signal a
    // passthrough-shaped corrector cannot accidentally zero out.
    for (float sample : out) {
        CHECK(sample == 0.0f);
    }
}

TEST_CASE("Engine: FR2 -- unvoiced (non-silent) audio passes through bit-identically") {
    // White noise, not silence: this has real, non-zero, non-repeating
    // energy, so a bit-identical match can only come from the unvoiced
    // bypass actually running (Engine.cpp's `!estimate.voiced` branch),
    // never by accident the way an all-zero signal could. A stub detector
    // fixes `voiced = false` regardless of the input's actual content --
    // white noise has no pitch to detect anyway, but the point of this
    // test is Engine's bypass logic, not AutocorrelationDetector's own
    // (separately tested) voicing decision.
    constexpr opentune::PitchEstimate kUnvoiced{0.0f, 0.0f, false};
    StubPitchDetector detector(kUnvoiced);
    opentune::ScaleQuantizer quantizer({0, opentune::ScaleType::Chromatic});
    opentune::ResampleCorrector corrector;
    opentune::Params params;

    opentune::Engine engine(detector, quantizer, corrector, params);

    constexpr int kBlockSamples = 256;
    engine.prepare(kSampleRate, kBlockSamples);
    engine.reset();

    const std::vector<float> in = opentune::test::whiteNoise(kBlockSamples, /*seed=*/12345u);
    std::vector<float> out(static_cast<std::size_t>(kBlockSamples), 0.0f);

    engine.process(in.data(), out.data(), kBlockSamples);

    // Exact equality holds here because ResampleCorrector is bit-transparent
    // at unity ratio. Since D6 the bypass routes through the corrector rather
    // than copying, so this is a property of THIS corrector, not of Engine:
    // a lossy corrector (SignalsmithCorrector's STFT) will not be sample-exact
    // here, and FR2 does not require it to be -- it requires *uncorrected*,
    // and unity ratio applies no correction. The latency-consistency test
    // below is what pins Engine's own behaviour.
    REQUIRE(out.size() == in.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
        CAPTURE(i);
        CHECK(out[i] == in[i]);
    }
}

TEST_CASE("Engine: FR2 -- the unvoiced bypass runs THROUGH the corrector (tasks.md D6)") {
    // The bug this pins: the bypass used to copy input straight to output,
    // giving it ZERO latency while corrected audio came out delayed by the
    // corrector's own latency. Two paths, two latencies, so at every
    // voiced/unvoiced boundary the bypassed audio arrived ahead of the
    // corrected audio it should have followed and overwrote its tail.
    //
    // Measured on the real pipeline before the fix, with a 300 ms silent
    // gap in a tone and a 140 ms-latency corrector: the gap came out 260 ms
    // long and 40 ms early. In singing, every consonant and every breath is
    // such a boundary.
    //
    // A stub corrector with a non-zero declared latency is the honest way to
    // test this: it lets us assert that the unvoiced path is routed through
    // the same component as the voiced path, which is what makes the two
    // latencies identical no matter which corrector is installed.
    constexpr opentune::PitchEstimate kUnvoiced{0.0f, 0.0f, false};
    StubPitchDetector detector(kUnvoiced);
    opentune::ScaleQuantizer quantizer({0, opentune::ScaleType::Chromatic});
    StubPitchCorrector corrector(/*latencySamples=*/6720); // 140 ms at 48 kHz
    opentune::Params params;

    opentune::Engine engine(detector, quantizer, corrector, params);

    constexpr int kBlockSamples = 256;
    engine.prepare(kSampleRate, kBlockSamples);
    engine.reset();

    const std::vector<float> in = opentune::test::whiteNoise(kBlockSamples, /*seed=*/999u);
    std::vector<float> out(static_cast<std::size_t>(kBlockSamples), 0.0f);

    engine.process(in.data(), out.data(), kBlockSamples);

    // The corrector must have been called even though the input is unvoiced.
    // Before the fix this was 0: the bypass returned without touching it.
    CHECK(corrector.callCount == 1);
    // ...and called at unity, so no pitch correction is applied (FR2).
    CHECK(corrector.lastRatio == doctest::Approx(1.0f));
    CHECK(corrector.lastN == kBlockSamples);

    // Engine's reported latency stays the corrector's, for voiced and
    // unvoiced alike -- there is only one path now, so there is one latency.
    CHECK(engine.latencySamples() == 6720);
}

TEST_CASE("Engine: process() delivers the quantize-derived ratio to the corrector") {
    // Finding 3/6 of task-T0.9-findings-r1.md: verify, in situ, that the
    // ratio Engine::process() computes (quantize -> compute ratio -> clamp)
    // is exactly what reaches the corrector, using a recording
    // PitchCorrector test double rather than exposing Engine's internal
    // clamp function as part of its public API.
    //
    // 890 Hz stands in for "a detector reporting a pitch skewed toward the
    // next octave" (docs/decisions/0003's documented high-leaning octave
    // bias) rather than an exact scale tone, so the quantize step actually
    // does something (890 -> 880, not an identity 880 -> 880). As
    // documented in docs/decisions/0004 and the file header above, this
    // ratio (880/890 ~= 0.9888) still lands well inside the clamp's bound
    // -- the Stage 0 ScaleQuantizer cannot currently produce anything else
    // -- so this test demonstrates correct *delivery* of the computed
    // ratio, not the clamp visibly altering a value.
    constexpr opentune::PitchEstimate kOctaveSkewedEstimate{890.0f, 1.0f, true};
    StubPitchDetector detector(kOctaveSkewedEstimate);
    opentune::ScaleQuantizer quantizer({0, opentune::ScaleType::Chromatic});
    StubPitchCorrector corrector;
    opentune::Params params;

    opentune::Engine engine(detector, quantizer, corrector, params);

    constexpr int kBlockSamples = 256;
    engine.prepare(kSampleRate, kBlockSamples);
    engine.reset();

    const std::vector<float> in(static_cast<std::size_t>(kBlockSamples), 0.25f);
    std::vector<float> out(static_cast<std::size_t>(kBlockSamples), 0.0f);
    engine.process(in.data(), out.data(), kBlockSamples);

    REQUIRE(corrector.callCount == 1);
    CHECK(corrector.lastN == kBlockSamples);

    const float expectedTargetHz = quantizer.snap(kOctaveSkewedEstimate.frequencyHz);
    const float expectedRatio = expectedTargetHz / kOctaveSkewedEstimate.frequencyHz;
    CAPTURE(expectedTargetHz);
    CAPTURE(expectedRatio);
    CHECK(corrector.lastRatio == doctest::Approx(expectedRatio));
}

TEST_CASE("Engine: latencySamples() forwards the injected corrector's latency") {
    constexpr opentune::PitchEstimate kUnvoiced{0.0f, 0.0f, false};
    StubPitchDetector detector(kUnvoiced);
    opentune::ScaleQuantizer quantizer({0, opentune::ScaleType::Chromatic});
    StubPitchCorrector corrector(/*latencySamples=*/1234);
    opentune::Params params;

    opentune::Engine engine(detector, quantizer, corrector, params);
    engine.prepare(kSampleRate, 256);

    CHECK(engine.latencySamples() == 1234);
}

TEST_CASE("Engine: same input via different block sizes gives the same output") {
    // Isolates the invariance check to Engine's own per-block wiring (and
    // the corrector's already-documented ratio<=1 chunk-invariance, see
    // ResampleCorrector.h) by using a stub detector that always reports
    // the same estimate regardless of chunking -- so any difference
    // between the two runs below can only come from a bug in Engine
    // itself, not from AutocorrelationDetector's own buffer-filling being
    // chunk-size sensitive (a separate, already-covered concern).
    constexpr opentune::PitchEstimate kFixedEstimate{445.0f, 1.0f, true};

    // Quantizer snaps 445 -> 440 (a semitone below), so the ratio Engine
    // computes is 440/445 ~= 0.9888, which is < 1.0 -- exactly the regime
    // ResampleCorrector documents as genuinely chunk-invariant (it never
    // needs a sample beyond what has already arrived), so this test
    // exercises Engine's wiring, not ResampleCorrector's known ratio > 1.0
    // chunking quirk.
    constexpr int kTotalSamples = 2000;
    const std::vector<float> in = opentune::test::sweep(80.0f, 1000.0f, kSampleRate, kTotalSamples);

    auto runInBlocksOf = [&](int blockSize) {
        StubPitchDetector detector(kFixedEstimate);
        opentune::ScaleQuantizer quantizer({0, opentune::ScaleType::Chromatic});
        opentune::ResampleCorrector corrector;
        opentune::Params params;

        opentune::Engine engine(detector, quantizer, corrector, params);
        engine.prepare(kSampleRate, 256);
        engine.reset();

        std::vector<float> out(static_cast<std::size_t>(kTotalSamples), 0.0f);
        int offset = 0;
        while (offset < kTotalSamples) {
            const int n = std::min(blockSize, kTotalSamples - offset);
            engine.process(in.data() + offset, out.data() + offset, n);
            offset += n;
        }
        return out;
    };

    const std::vector<float> outSmallBlocks = runInBlocksOf(37);
    const std::vector<float> outLargeBlocks = runInBlocksOf(256);

    REQUIRE(outSmallBlocks.size() == outLargeBlocks.size());
    for (std::size_t i = 0; i < outSmallBlocks.size(); ++i) {
        CAPTURE(i);
        CHECK(outSmallBlocks[i] == outLargeBlocks[i]);
    }
}

TEST_CASE("Engine: KNOWN LIMITATION (tasks.md D5) -- a ratio > 1.0 correction "
          "(flat input) degenerates into a block-rate staircase after ~0.68 s") {
    // Finding 4 of task-T0.9-findings-r1.md: every other test in this file
    // corrects *downward* (ratio < 1.0). This test documents, rather than
    // avoids, the other half of ResampleCorrector's known bad behaviour
    // (T0.7's deliberate naive implementation, not a T0.9 defect -- the
    // remedy is being decided by ResampleCorrector's owner, tracked as
    // tasks.md D5): above ratio 1.0, ResampleCorrector's read position
    // outruns the input it has actually been given and it falls back to
    // holding (repeating) its last available sample once it runs out.
    //
    // A detector fixed at 435 Hz is a semitone-ish flat of A4; the
    // chromatic quantizer snaps it up to 440 Hz, giving ratio 440/435 ~=
    // 1.0115 -- controller-measured (task-T0.9-findings-r1.md finding 4) to
    // produce its first fully block-constant output block at block index
    // 128 (sample 32768, ~0.68 s at 48 kHz/256-sample blocks), with the
    // held value at full scale (peak ~0.5) rather than silence.
    constexpr opentune::PitchEstimate kFlatEstimate{435.0f, 1.0f, true};
    StubPitchDetector detector(kFlatEstimate);
    opentune::ScaleQuantizer quantizer({0, opentune::ScaleType::Chromatic});
    opentune::ResampleCorrector corrector;
    opentune::Params params;

    opentune::Engine engine(detector, quantizer, corrector, params);

    constexpr int kBlockSize = 256;
    engine.prepare(kSampleRate, kBlockSize);
    engine.reset();

    // Run well past the documented collapse point (block 128) so both the
    // healthy region beforehand and the degenerate region afterward are
    // observed in one run.
    constexpr int kNumBlocks = 200;
    constexpr int kTotalSamples = kNumBlocks * kBlockSize;
    const std::vector<float> in = opentune::test::sine(435.0f, kSampleRate, kTotalSamples);
    std::vector<float> out(static_cast<std::size_t>(kTotalSamples), 0.0f);

    for (int block = 0; block < kNumBlocks; ++block) {
        const std::size_t offset = static_cast<std::size_t>(block) * kBlockSize;
        engine.process(in.data() + offset, out.data() + offset, kBlockSize);
    }

    // Before collapse (well clear of block 128, with margin): a real,
    // still-varying corrected waveform -- not yet degenerate.
    constexpr std::size_t kHealthyBlock = 50;
    CHECK_FALSE(isConstantBlock(out, kHealthyBlock * kBlockSize, kBlockSize));

    // After collapse (with margin past the documented block 128): the
    // known-bad, pinned behaviour -- a fully constant-valued block...
    constexpr std::size_t kDegenerateBlock = 150;
    const std::size_t degenerateStart = kDegenerateBlock * kBlockSize;
    CHECK(isConstantBlock(out, degenerateStart, kBlockSize));

    // ...held at full scale, not silence -- the specific, surprising
    // failure mode this test exists to pin down (a naive "hold the last
    // sample" bug that degrades to silence would be far less alarming than
    // one that degrades to a loud, block-rate square-wave-like staircase).
    const float heldValue = out[degenerateStart];
    CAPTURE(heldValue);
    CHECK(std::abs(heldValue) > 0.4f);
}

// constitution II / engine/CLAUDE.md: process(), reset(), and
// latencySamples() are real-time functions and must never allocate,
// throw, lock, log, or block -- noexcept is the compiler-enforced half of
// that contract.
static_assert(noexcept(std::declval<opentune::Engine&>().process(nullptr, nullptr, 0)),
              "Engine::process must be noexcept (audio thread, constitution II)");
static_assert(noexcept(std::declval<opentune::Engine&>().reset()),
              "Engine::reset must be noexcept (audio thread, constitution II)");
static_assert(noexcept(std::declval<const opentune::Engine&>().latencySamples()),
              "Engine::latencySamples must be noexcept (audio thread, constitution II)");
