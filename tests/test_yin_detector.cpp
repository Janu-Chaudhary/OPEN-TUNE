// Placeholder registered ahead of the Stage 1 task that owns this file, so
// that tests/CMakeLists.txt has exactly one writer while several implementers
// run concurrently. The owning task replaces this with real failing tests.
#include "doctest.h"

TEST_CASE("test_yin_detector is registered") {
    CHECK(true);
}
