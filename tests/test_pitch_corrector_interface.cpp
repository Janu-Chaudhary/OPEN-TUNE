#include "doctest.h"
#include "opentune/PitchCorrector.h"

#include <memory>
#include <utility>
#include <vector>

// T0.6 done-criterion: PitchCorrector is an abstract interface that a stub
// pass-through subclass can implement and be driven entirely through the
// base-class pointer. This does not test any real pitch-shifting algorithm
// (there is none yet) -- it only proves the interface's shape is usable.
namespace {

// A stub corrector: ignores `pitchRatio` entirely and copies `in` to `out`
// unchanged. This is enough to prove the interface can be implemented and
// driven polymorphically; a real corrector (e.g. a resampling corrector,
// then Signalsmith Stretch) will replace this in a later task without
// changing this contract.
class StubPassThroughCorrector final : public opentune::PitchCorrector {
public:
    void prepare(double /*sampleRate*/, int /*maxBlockSize*/) override {
        // Naive stub: nothing to allocate. A real corrector would size its
        // internal delay line / analysis buffers here.
    }

    void reset() noexcept override {
        // Naive stub: no internal state to clear.
    }

    void process(const float* in, float* out, int n, float /*pitchRatio*/) noexcept override {
        for (int i = 0; i < n; ++i) {
            out[static_cast<std::size_t>(i)] = in[static_cast<std::size_t>(i)];
        }
    }

    int latencySamples() const noexcept override {
        // A pass-through stub introduces no delay: it never buffers samples
        // before emitting them.
        return 0;
    }
};

} // namespace

TEST_CASE("PitchCorrector interface is usable through a base-class pointer") {
    std::unique_ptr<opentune::PitchCorrector> corrector =
        std::make_unique<StubPassThroughCorrector>();

    corrector->prepare(48000.0, 256);
    corrector->reset();

    // A 256-sample ramp: distinct, non-trivial values so a broken
    // copy/indexing bug (e.g. an off-by-one, or writing zeros) would be
    // caught, unlike an all-zero or all-constant block.
    constexpr int kBlockSize = 256;
    std::vector<float> in(static_cast<std::size_t>(kBlockSize));
    for (int i = 0; i < kBlockSize; ++i) {
        in[static_cast<std::size_t>(i)] = static_cast<float>(i);
    }
    std::vector<float> out(static_cast<std::size_t>(kBlockSize), -1.0f);

    corrector->process(in.data(), out.data(), kBlockSize, 1.0f);

    CHECK(out == in);
    CHECK(corrector->latencySamples() == 0);
}

// constitution II / engine/CLAUDE.md: process() and latencySamples() are
// real-time functions and must never allocate, throw, lock, log, or block --
// noexcept is the compiler-enforced half of that contract. These assertions
// fail to compile (rather than failing at runtime) if a future
// implementation drops `noexcept` from an override, which is exactly when we
// want to catch it. They test the interface itself, not any stub override.
static_assert(noexcept(std::declval<opentune::PitchCorrector&>().process(nullptr, nullptr, 0,
                                                                         1.0f)),
              "PitchCorrector::process must be noexcept (audio thread, constitution II)");
static_assert(noexcept(std::declval<const opentune::PitchCorrector&>().latencySamples()),
              "PitchCorrector::latencySamples must be noexcept (audio thread, constitution II)");
