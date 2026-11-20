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

  SUBCASE("net_allows fails closed on an unset or too-broad subnet") {
    CHECK_FALSE(net_allows(Subnet{}, 0xC0A80132u));           // zero subnet denies all
    CHECK_FALSE(net_allows(Subnet{0xC0A8010Au, 0u}, 0xC0A80132u));  // mask 0 denies
    CHECK_FALSE(net_allows(Subnet{0xC0A8010Au, 0xF0000000u}, 0xC0A80132u));  // /4 too broad
  }

  SUBCASE("net_allows denies the hub itself, network and broadcast") {
    CHECK_FALSE(net_allows(local, 0xC0A8010Au));  // the hub's own ip (SSRF to host)
    CHECK_FALSE(net_allows(local, 0xC0A80100u));  // network address .0
    CHECK_FALSE(net_allows(local, 0xC0A801FFu));  // broadcast .255
    CHECK(net_allows(local, 0xC0A80132u));        // an ordinary peer still allowed
  }

  SUBCASE("check_net denies reaching the hub even with the capability") {
    CapSet net;
    net.add(Cap::NetLocal);
    CHECK(check_net(net, local, 0xC0A8010Au) == KsErr::Denied);  // hub's own ip
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
