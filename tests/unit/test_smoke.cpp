// Phase 1 smoke test.
//
// Purpose: establish that (a) the test executable builds, (b) the doctest
// runner starts, (c) a basic assertion succeeds, and (d) the toolchain is
// actually compiling as C++20. Functional tests are added by the phases
// that implement the corresponding functionality.

#include <doctest.h>

TEST_CASE("smoke: scaffold builds, runner starts, assertions work") {
    CHECK(true);
    CHECK(1 + 1 == 2);
    CHECK(__cplusplus >= 202002L); // C++20 must be active, not an older fallback
}
