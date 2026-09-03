#include "doctest.h"
#include "opentune/Version.h"

#include <cstring>

// T0.1 done-criterion: the build system, the engine library and the test
// runner are wired together. If this test runs, the skeleton is alive.
TEST_CASE("engine reports a semantic version") {
    const char* v = opentune::version();
    REQUIRE(v != nullptr);
    CHECK(std::strlen(v) >= 5); // "0.1.0" is the shortest valid form
    CHECK(std::strcmp(v, "0.1.0") == 0);
}
