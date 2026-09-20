#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <vector>

#include "core/entity_registry.hpp"
#include "core/in_memory_link.hpp"
#include "core/radio_coordinator.hpp"
#include "core/uart_transport.hpp"
#include "hubproto/message.hpp"
#include "hubproto/payload.hpp"
#include "hubproto/version.hpp"

using core::EntityRegistry;
using core::InMemoryLink;
using core::UartTransport;
using hubproto::AttrDecl;
using hubproto::DeviceType;
using hubproto::Header;
using hubproto::MsgType;
using hubproto::ValueType;

namespace {

using Registry = EntityRegistry<>;

Header mk(MsgType type, std::uint8_t chip_id, std::uint8_t boot_id) {
  Header h{};
  h.version = hubproto::kProtocolVersion;
  h.type = type;
  h.chip_id = chip_id;
  h.boot_id = boot_id;
  h.seq = 0;
  h.entity_id = hubproto::kSelfEntityId;
  return h;
}

// Records inbound messages the "chip" side sees (e.g. QUERY from the brain).
struct Capture {
  int count = 0;
  Header last{};
  static void on(void* ctx, const Header& h, const std::uint8_t*, std::size_t) {
    auto* c = static_cast<Capture*>(ctx);
    ++c->count;
    c->last = h;
  }
};

const std::array<AttrDecl, 2> kLight{{
    {0x01, ValueType::Bool, 0x07},
    {0x02, ValueType::U8, 0x07},
}};
const std::array<AttrDecl, 1> kSwitch{{{0x01, ValueType::Bool, 0x07}}};

// Publish a DECLARE for one entity from the chip side.
void publish_declare(UartTransport& chip, std::uint8_t chip_id,
                     std::uint16_t entity_id, DeviceType dt,
                     const AttrDecl* attrs, std::uint8_t count) {
  std::array<std::uint8_t, 64> buf{};
  auto r = hubproto::encode_declare(entity_id, dt, attrs, count, buf.data(),
                                    buf.size());
  REQUIRE(r.ok);
  chip.publish(mk(MsgType::Declare, chip_id, 1), buf.data(), r.len);
}

}  // namespace

TEST_CASE("HELLO triggers a QUERY back to the chip") {
  InMemoryLink la, lb;
  InMemoryLink::connect(la, lb);
  UartTransport brain(la);
  UartTransport chip(lb);
  Registry reg;
  core::RadioCoordinator<Registry> coord(brain, reg);

  Capture at_chip;
  chip.subscribe(&Capture::on, &at_chip);

  chip.publish(mk(MsgType::Hello, 0x01, 1), nullptr, 0);

  REQUIRE(at_chip.count == 1);
  CHECK(at_chip.last.type == MsgType::Query);
  CHECK(at_chip.last.chip_id == 0x01);
  CHECK(coord.chip_known(0x01));
  CHECK_FALSE(coord.chip_ready(0x01));
}

TEST_CASE("DECLARE cycle populates the registry and marks the chip ready") {
  InMemoryLink la, lb;
  InMemoryLink::connect(la, lb);
  UartTransport brain(la);
  UartTransport chip(lb);
  Registry reg;
  core::RadioCoordinator<Registry> coord(brain, reg);

  chip.publish(mk(MsgType::Hello, 0x01, 1), nullptr, 0);
  publish_declare(chip, 0x01, 0x0010, DeviceType::Light, kLight.data(), 2);
  publish_declare(chip, 0x01, 0x0020, DeviceType::Switch, kSwitch.data(), 1);
  chip.publish(mk(MsgType::DeclareEnd, 0x01, 1), nullptr, 0);

  CHECK(reg.size() == 2);
  const auto* light = reg.find(0x0010);
  REQUIRE(light != nullptr);
  CHECK(light->device_type == DeviceType::Light);
  CHECK(light->chip_id == 0x01);
  CHECK(reg.find(0x0020)->device_type == DeviceType::Switch);
  CHECK(coord.chip_ready(0x01));
}

TEST_CASE("a new boot_id resyncs: entities cleared and re-queried") {
  InMemoryLink la, lb;
  InMemoryLink::connect(la, lb);
  UartTransport brain(la);
  UartTransport chip(lb);
  Registry reg;
  core::RadioCoordinator<Registry> coord(brain, reg);

  chip.publish(mk(MsgType::Hello, 0x01, 1), nullptr, 0);
  publish_declare(chip, 0x01, 0x0010, DeviceType::Light, kLight.data(), 2);
  chip.publish(mk(MsgType::DeclareEnd, 0x01, 1), nullptr, 0);
  REQUIRE(reg.size() == 1);
  REQUIRE(coord.chip_ready(0x01));

  Capture at_chip;
  chip.subscribe(&Capture::on, &at_chip);

  // Chip reboots and re-announces with a new boot_id.
  chip.publish(mk(MsgType::Hello, 0x01, 2), nullptr, 0);

  CHECK(reg.size() == 0);                 // stale entities dropped
  CHECK_FALSE(coord.chip_ready(0x01));    // back to declaring
  REQUIRE(at_chip.count == 1);
  CHECK(at_chip.last.type == MsgType::Query);
}

TEST_CASE("an unknown chip is queried on any message (brain reboot)") {
  InMemoryLink la, lb;
  InMemoryLink::connect(la, lb);
  UartTransport brain(la);
  UartTransport chip(lb);
  Registry reg;
  core::RadioCoordinator<Registry> coord(brain, reg);

  Capture at_chip;
  chip.subscribe(&Capture::on, &at_chip);

  // The chip was already running; the brain just booted and sees a HEARTBEAT.
  chip.publish(mk(MsgType::Heartbeat, 0x01, 5), nullptr, 0);

  REQUIRE(at_chip.count == 1);
  CHECK(at_chip.last.type == MsgType::Query);
  CHECK(coord.chip_known(0x01));
}

TEST_CASE("a changed boot_id on a HEARTBEAT resyncs (self-healing)") {
  InMemoryLink la, lb;
  InMemoryLink::connect(la, lb);
  UartTransport brain(la);
  UartTransport chip(lb);
  Registry reg;
  core::RadioCoordinator<Registry> coord(brain, reg);

  chip.publish(mk(MsgType::Hello, 0x01, 1), nullptr, 0);
  publish_declare(chip, 0x01, 0x0010, DeviceType::Light, kLight.data(), 2);
  chip.publish(mk(MsgType::DeclareEnd, 0x01, 1), nullptr, 0);
  REQUIRE(reg.size() == 1);
  REQUIRE(coord.chip_ready(0x01));

  Capture at_chip;
  chip.subscribe(&Capture::on, &at_chip);

  // Chip rebooted (new boot_id) but we notice via a HEARTBEAT, not a fresh HELLO.
  chip.publish(mk(MsgType::Heartbeat, 0x01, 2), nullptr, 0);

  CHECK(reg.size() == 0);
  CHECK_FALSE(coord.chip_ready(0x01));
  REQUIRE(at_chip.count == 1);
  CHECK(at_chip.last.type == MsgType::Query);
}

TEST_CASE("a HEARTBEAT with an unchanged boot_id does not disturb the registry") {
  InMemoryLink la, lb;
  InMemoryLink::connect(la, lb);
  UartTransport brain(la);
  UartTransport chip(lb);
  Registry reg;
  core::RadioCoordinator<Registry> coord(brain, reg);

  chip.publish(mk(MsgType::Hello, 0x01, 1), nullptr, 0);
  publish_declare(chip, 0x01, 0x0010, DeviceType::Light, kLight.data(), 2);
  chip.publish(mk(MsgType::DeclareEnd, 0x01, 1), nullptr, 0);
  REQUIRE(reg.size() == 1);

  Capture at_chip;
  chip.subscribe(&Capture::on, &at_chip);

  chip.publish(mk(MsgType::Heartbeat, 0x01, 1), nullptr, 0);  // same boot_id

  CHECK(reg.size() == 1);
  CHECK(coord.chip_ready(0x01));
  CHECK(at_chip.count == 0);  // no spurious QUERY
}

TEST_CASE("resync clears only the rebooting chip's entities") {
  InMemoryLink la, lb;
  InMemoryLink::connect(la, lb);
  UartTransport brain(la);
  UartTransport chip(lb);
  Registry reg;
  core::RadioCoordinator<Registry> coord(brain, reg);

  chip.publish(mk(MsgType::Hello, 0x01, 1), nullptr, 0);
  publish_declare(chip, 0x01, 0x0010, DeviceType::Light, kLight.data(), 2);
  chip.publish(mk(MsgType::DeclareEnd, 0x01, 1), nullptr, 0);

  chip.publish(mk(MsgType::Hello, 0x02, 1), nullptr, 0);
  publish_declare(chip, 0x02, 0x0020, DeviceType::Switch, kSwitch.data(), 1);
  chip.publish(mk(MsgType::DeclareEnd, 0x02, 1), nullptr, 0);
  REQUIRE(reg.size() == 2);

  chip.publish(mk(MsgType::Hello, 0x01, 2), nullptr, 0);  // only chip 1 reboots
  CHECK(reg.size() == 1);
  CHECK(reg.find(0x0010) == nullptr);
  CHECK(reg.find(0x0020) != nullptr);
}
