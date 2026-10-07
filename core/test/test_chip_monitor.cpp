#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include "core/chip_monitor.hpp"
#include "core/in_memory_link.hpp"
#include "core/uart_transport.hpp"
#include "hubproto/message.hpp"
#include "hubproto/version.hpp"

using core::ChipMonitor;
using core::InMemoryLink;
using core::UartTransport;
using hubproto::Header;
using hubproto::MsgType;

namespace {

struct Clock {
  std::uint32_t t = 0;
  static std::uint32_t now(void* c) { return static_cast<Clock*>(c)->t; }
};

struct Ev {
  std::uint8_t chip;
  ChipMonitor::Event ev;
  std::uint32_t value;
};
struct EvLog {
  std::vector<Ev> v;
  static void on(void* c, std::uint8_t chip, ChipMonitor::Event e, std::uint32_t val) {
    static_cast<EvLog*>(c)->v.push_back({chip, e, val});
  }
};

// The chip side: send a heartbeat stamped with chip/boot id; count QUERYs.
struct Chip {
  UartTransport* bus;
  std::uint8_t id;
  int queries = 0;
  static void on(void* c, const Header& h, const std::uint8_t*, std::size_t) {
    if (h.type == MsgType::Query) ++static_cast<Chip*>(c)->queries;
  }
  void beat(std::uint8_t boot_id) {
    Header h{};
    h.version = hubproto::kProtocolVersion;
    h.type = MsgType::Heartbeat;
    h.chip_id = id;
    h.boot_id = boot_id;
    bus->publish(h, nullptr, 0);
  }
};

struct Rig {
  InMemoryLink la, lb;
  UartTransport brain{la}, chipbus{lb};
  Clock clock;
  EvLog log;
  Chip chip{&chipbus, 0x01};
  Rig() {
    InMemoryLink::connect(la, lb);
    chipbus.subscribe(&Chip::on, &chip);
  }
};

}  // namespace

TEST_CASE("a chip that keeps talking stays alive; one that stops goes silent and is probed") {
  Rig r;
  ChipMonitor mon(r.brain, &Clock::now, &r.clock);
  mon.on_event(&EvLog::on, &r.log);

  CHECK(mon.alive(0x01));  // never heard from: not blocked
  r.chip.beat(1);
  CHECK(mon.info(0x01) != nullptr);
  r.clock.t = 15000;
  mon.poll(r.clock.t);
  CHECK(mon.alive(0x01));
  CHECK(r.log.v.empty());

  r.clock.t = 20000;
  mon.poll(r.clock.t);
  CHECK_FALSE(mon.alive(0x01));
  REQUIRE(r.log.v.size() == 1);
  CHECK(r.log.v[0].ev == ChipMonitor::Event::Lost);
  CHECK(r.log.v[0].value == 20000);

  // Probed with a QUERY every probe interval, not every poll.
  r.clock.t = 25000;
  mon.poll(r.clock.t);
  CHECK(r.chip.queries == 0);
  r.clock.t = 30000;
  mon.poll(r.clock.t);
  CHECK(r.chip.queries == 1);
  r.clock.t = 31000;
  mon.poll(r.clock.t);
  CHECK(r.chip.queries == 1);

  // It answers again: resumed, alive, same boot_id -> not a reboot.
  r.clock.t = 32000;
  r.chip.beat(1);
  CHECK(mon.alive(0x01));
  REQUIRE(r.log.v.size() == 2);
  CHECK(r.log.v[1].ev == ChipMonitor::Event::Resumed);
  CHECK(r.log.v[1].value == 32000);
  CHECK(mon.info(0x01)->reboots == 0);
}

TEST_CASE("a changed boot_id is a reboot; repeated reboots are flapping") {
  Rig r;
  ChipMonitor mon(r.brain, &Clock::now, &r.clock);
  mon.on_event(&EvLog::on, &r.log);

  r.chip.beat(5);
  r.chip.beat(5);
  CHECK(r.log.v.empty());

  r.clock.t = 1000;
  r.chip.beat(6);
  REQUIRE(r.log.v.size() == 1);
  CHECK(r.log.v[0].ev == ChipMonitor::Event::Rebooted);
  CHECK(r.log.v[0].value == 1);

  r.clock.t = 2000;
  r.chip.beat(7);  // 2 reboots in the window: below the threshold of 3
  CHECK(r.log.v.size() == 2);

  r.clock.t = 3000;
  r.chip.beat(8);  // 3rd: flapping
  REQUIRE(r.log.v.size() == 4);
  CHECK(r.log.v[2].ev == ChipMonitor::Event::Rebooted);
  CHECK(r.log.v[3].ev == ChipMonitor::Event::Flapping);
  CHECK(r.log.v[3].value == 3);
  CHECK(mon.recent_reboots(0x01, 3000) == 3);
  CHECK(mon.recent_reboots(0x01, 3000 + 400000) == 0);  // aged out of the window
}

TEST_CASE("an unstamped message (boot_id 0) is not a reboot") {
  Rig r;
  ChipMonitor mon(r.brain, &Clock::now, &r.clock);
  mon.on_event(&EvLog::on, &r.log);
  r.chip.beat(3);
  r.chip.beat(0);
  r.chip.beat(3);
  CHECK(r.log.v.empty());
  CHECK(mon.info(0x01)->reboots == 0);
}

TEST_CASE("chips are tracked independently") {
  Rig r;
  Chip other{&r.chipbus, 0x02};
  ChipMonitor mon(r.brain, &Clock::now, &r.clock);
  r.chip.beat(1);
  other.beat(1);
  r.clock.t = 19000;
  other.beat(1);
  r.clock.t = 21000;
  mon.poll(r.clock.t);
  CHECK_FALSE(mon.alive(0x01));
  CHECK(mon.alive(0x02));
}
