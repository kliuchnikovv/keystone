#include <doctest/doctest.h>

#include "core/plugin_host_api.hpp"

TEST_CASE("plugin host-api: cap set add/has and abi version") {
  core::plugin::CapSet caps;
  CHECK_FALSE(caps.has(core::plugin::Cap::NetLocal));

  caps.add(core::plugin::Cap::NetLocal);
  CHECK(caps.has(core::plugin::Cap::NetLocal));
  CHECK_FALSE(caps.has(core::plugin::Cap::BusRead));

  CHECK(core::plugin::kAbiVersion == 1u);
  CHECK(static_cast<int>(core::plugin::KsErr::Ok) == 0);
}
