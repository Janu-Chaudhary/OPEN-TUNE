#include "doctest.h"
#include "opentune/Params.h"

#include <type_traits>

// T0.8 done-criteria (tasks.md / specs.md section 7.1): Params compiles, and every
// default value matches specs.md section 7.1 exactly:
//   strength = 0.8f, retuneMs = 40.0f, humanizeCents = 15.0f,
//   preserveFormants = true, scale = { 0, ScaleType::Chromatic }.

TEST_CASE("Params: default strength is 0.8 (mostly hard-tuned, but not full snap)") {
    opentune::Params params;
    CHECK(params.strength == doctest::Approx(0.8f));
}

TEST_CASE("Params: default retuneMs is 40.0 (fast but not instant retune)") {
    opentune::Params params;
    CHECK(params.retuneMs == doctest::Approx(40.0f));
}

TEST_CASE("Params: default humanizeCents is 15.0 (a little residual imperfection)") {
    opentune::Params params;
    CHECK(params.humanizeCents == doctest::Approx(15.0f));
}

TEST_CASE("Params: default preserveFormants is true (no chipmunk artifact by default)") {
    opentune::Params params;
    CHECK(params.preserveFormants == true);
}

TEST_CASE("Params: default scale is chromatic rooted at MIDI 0") {
    opentune::Params params;
    CHECK(params.scale.rootMidiNote == 0);
    CHECK(params.scale.type == opentune::ScaleType::Chromatic);
}

// Params is handed from a UI/control thread to the audio thread by value (see
// Params.h). That handoff must be safe to do with a plain memory copy -- no
// constructor running, no hidden pointer/ownership semantics that a bitwise
// copy would violate (e.g. a self-referential pointer, or a heap allocation
// that a naive copy would double-free). std::is_trivially_copyable_v verifies
// exactly that: Params has no user-defined copy/move/destructor and every
// member is itself trivially copyable, so copying it is just copying bytes.
static_assert(std::is_trivially_copyable_v<opentune::Params>,
              "Params is copied by value onto the audio thread; it must be safe to copy as "
              "plain bytes, with no constructor/destructor logic to run.");
