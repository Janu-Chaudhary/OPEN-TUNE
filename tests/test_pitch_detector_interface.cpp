#include "doctest.h"
#include "opentune/PitchDetector.h"

#include <memory>
#include <utility>
#include <vector>

// T0.3 done-criterion: PitchDetector is an abstract interface that a stub
// subclass can implement and be driven entirely through the base-class
// pointer. This does not test any real pitch-detection algorithm (there is
// none yet) -- it only proves the interface's shape is usable.
namespace {

// A stub detector: ignores its input entirely and always reports the same
// fixed PitchEstimate. This is enough to prove the interface can be
// implemented and driven polymorphically; a real detector (e.g. YIN) will
// replace this in a later task without changing this contract.
class StubPitchDetector final : public opentune::PitchDetector {
public:
    void prepare(double /*sampleRate*/, int /*maxBlockSize*/) override {
        // Naive stub: nothing to allocate. A real detector would size its
        // analysis ring buffer here.
    }

    void reset() noexcept override {
        // Naive stub: no internal state to clear.
    }

    opentune::PitchEstimate process(const float* /*block*/, int /*n*/) noexcept override {
        return kFixedEstimate;
    }

private:
    static constexpr opentune::PitchEstimate kFixedEstimate{440.0f, 1.0f, true};
};

} // namespace

TEST_CASE("PitchDetector interface is usable through a base-class pointer") {
    std::unique_ptr<opentune::PitchDetector> detector = std::make_unique<StubPitchDetector>();

    detector->prepare(48000.0, 256);
    detector->reset();

    const std::vector<float> block(256, 0.0f);
    const opentune::PitchEstimate estimate = detector->process(block.data(), 256);

    CHECK(estimate.frequencyHz == doctest::Approx(440.0f));
    CHECK(estimate.confidence == doctest::Approx(1.0f));
    CHECK(estimate.voiced == true);
}

// constitution II / engine/CLAUDE.md: process() is a real-time function and
// must never allocate, throw, lock, log, or block -- noexcept is the
// compiler-enforced half of that contract. This assertion fails to compile
// (rather than failing at runtime) if a future implementation drops
// `noexcept` from the override, which is exactly when we want to catch it.
static_assert(noexcept(std::declval<opentune::PitchDetector&>().process(nullptr, 0)),
              "PitchDetector::process must be noexcept (audio thread, constitution II)");
