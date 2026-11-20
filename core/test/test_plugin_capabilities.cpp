#include <doctest/doctest.h>

#include "core/plugin_capabilities.hpp"
#include "core/plugin_host_api.hpp"

TEST_CASE("plugin caps: net local-only and entity access") {
  using namespace core::plugin;

  // 192.168.1.10/24
  const Subnet local{0xC0A8010Au, 0xFFFFFF00u};

  SUBCASE("net_allows: same subnet yes, outside no") {
    CHECK(net_allows(local, 0xC0A80132u));   // 192.168.1.50
    CHECK_FALSE(net_allows(local, 0x08080808u));  // 8.8.8.8
  }

  SUBCASE("check_net gates on the NetLocal capability") {
    CapSet none;
    CHECK(check_net(none, local, 0xC0A80132u) == KsErr::Denied);  // local but no cap

    CapSet net;
    net.add(Cap::NetLocal);
    CHECK(check_net(net, local, 0xC0A80132u) == KsErr::Ok);       // local + cap
    CHECK(check_net(net, local, 0x08080808u) == KsErr::Denied);   // non-local + cap
  }

  SUBCASE("entity_allowed: own always, others only if listed") {
    const uint16_t allow[] = {0x0200u};
    EntityAccess acc;
    acc.allow = allow;
    acc.allow_n = 1;

    CHECK(entity_allowed(acc, 0x0100u, /*is_owned=*/true));
    CHECK_FALSE(entity_allowed(acc, 0x0300u, /*is_owned=*/false));
    CHECK(entity_allowed(acc, 0x0200u, /*is_owned=*/false));
  }
}
