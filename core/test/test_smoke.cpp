// Phase 0 smoke test: proves core builds, links against libhubproto, and runs
// under ctest. Real registry/state-machine/rule tests arrive in Phases 3-5.
#include <doctest/doctest.h>

#include "core/core.hpp"

TEST_CASE("core links and reports ready") {
  CHECK(core::ready());
}
