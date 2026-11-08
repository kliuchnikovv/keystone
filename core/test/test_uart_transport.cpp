#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <vector>

#include "core/in_memory_link.hpp"
#include "core/uart_transport.hpp"
#include "hubproto/message.hpp"
#include "hubproto/payload.hpp"
#include "hubproto/version.hpp"

using core::InMemoryLink;
using core::UartTransport;
using hubproto::Header;
using hubproto::MsgType;

namespace {

// Records inbound messages for assertions.
struct Capture {
  int count = 0;
  Header last{};
  std::vector<std::uint8_t> payload;

  static void on_msg(void* ctx, const Header& h, const std::uint8_t* p,
                     std::size_t n) {
    auto* c = static_cast<Capture*>(ctx);
    ++c->count;
    c->last = h;
    c->payload.assign(p, p + n);
  }
};

Header hello(std::uint8_t chip_id, std::uint8_t boot_id) {
  Header h{};
  h.version = hubproto::kProtocolVersion;
  h.type = MsgType::Hello;
  h.chip_id = chip_id;
  h.boot_id = boot_id;
  h.seq = 0;
  h.entity_id = hubproto::kSelfEntityId;
  return h;
}

}  // namespace

TEST_CASE("publish on one side delivers a decoded message to the other") {
  InMemoryLink la, lb;
  InMemoryLink::connect(la, lb);
  UartTransport brain(la);
  UartTransport chip(lb);

  Capture at_brain;
  REQUIRE(brain.subscribe(&Capture::on_msg, &at_brain));

  Header h = hello(0x03, 0x09);
  REQUIRE(chip.publish(h, nullptr, 0));

  REQUIRE(at_brain.count == 1);
  CHECK(at_brain.last.type == MsgType::Hello);
  CHECK(at_brain.last.chip_id == 0x03);
  CHECK(at_brain.last.boot_id == 0x09);
  CHECK(at_brain.payload.empty());
}

TEST_CASE("a message with a payload round-trips through the bus") {
  InMemoryLink la, lb;
  InMemoryLink::connect(la, lb);
  UartTransport brain(la);
  UartTransport chip(lb);

  Capture at_brain;
  brain.subscribe(&Capture::on_msg, &at_brain);

  Header h{};
  h.version = 0x01;
  h.type = MsgType::State;
  h.chip_id = 0x03;
  h.boot_id = 0x01;
  h.seq = 0x00;
  h.entity_id = 0x0007;
  const std::array<std::uint8_t, 5> payload{0x07, 0x00, 0x01, 0x10, 0xCE};

  REQUIRE(chip.publish(h, payload.data(), payload.size()));
  REQUIRE(at_brain.count == 1);
  CHECK(at_brain.last.entity_id == 0x0007);
  REQUIRE(at_brain.payload.size() == payload.size());
  for (std::size_t i = 0; i < payload.size(); ++i) {
    CHECK(at_brain.payload[i] == payload[i]);
  }
}

TEST_CASE("the bus is bidirectional") {
  InMemoryLink la, lb;
  InMemoryLink::connect(la, lb);
  UartTransport brain(la);
  UartTransport chip(lb);

  Capture at_brain, at_chip;
  brain.subscribe(&Capture::on_msg, &at_brain);
  chip.subscribe(&Capture::on_msg, &at_chip);

  Header q{};
  q.version = 0x01;
  q.type = MsgType::Query;
  q.chip_id = 0x03;
  REQUIRE(brain.publish(q, nullptr, 0));  // brain -> chip
  CHECK(at_chip.count == 1);
  CHECK(at_chip.last.type == MsgType::Query);

  REQUIRE(chip.publish(hello(0x03, 0x02), nullptr, 0));  // chip -> brain
  CHECK(at_brain.count == 1);
  CHECK(at_brain.last.type == MsgType::Hello);
}

TEST_CASE("two messages arrive in order") {
  InMemoryLink la, lb;
  InMemoryLink::connect(la, lb);
  UartTransport brain(la);
  UartTransport chip(lb);

  std::vector<MsgType> seen;
  struct Rec {
    std::vector<MsgType>* out;
    static void on(void* ctx, const Header& h, const std::uint8_t*, std::size_t) {
      static_cast<Rec*>(ctx)->out->push_back(h.type);
    }
  } rec{&seen};
  brain.subscribe(&Rec::on, &rec);

  Header a = hello(0x03, 0x01);
  Header b{};
  b.version = 0x01;
  b.type = MsgType::Heartbeat;
  b.chip_id = 0x03;
  REQUIRE(chip.publish(a, nullptr, 0));
  REQUIRE(chip.publish(b, nullptr, 0));

  REQUIRE(seen.size() == 2);
  CHECK(seen[0] == MsgType::Hello);
  CHECK(seen[1] == MsgType::Heartbeat);
}

TEST_CASE("all subscribers are notified; the table has a bounded size") {
  InMemoryLink la, lb;
  InMemoryLink::connect(la, lb);
  UartTransport brain(la);
  UartTransport chip(lb);

  Capture c1, c2;
  REQUIRE(brain.subscribe(&Capture::on_msg, &c1));
  REQUIRE(brain.subscribe(&Capture::on_msg, &c2));

  // Fill the remaining slots, then expect a rejection.
  Capture filler;
  int accepted = 2;
  for (int i = 2; i < UartTransport::kMaxSubscribers; ++i) {
    if (brain.subscribe(&Capture::on_msg, &filler)) ++accepted;
  }
  CHECK(accepted == UartTransport::kMaxSubscribers);
  CHECK_FALSE(brain.subscribe(&Capture::on_msg, &filler));

  REQUIRE(chip.publish(hello(0x03, 0x01), nullptr, 0));
  CHECK(c1.count == 1);
  CHECK(c2.count == 1);
}

TEST_CASE("publish rejects an oversized payload") {
  InMemoryLink la, lb;
  InMemoryLink::connect(la, lb);
  UartTransport chip(lb);

  std::vector<std::uint8_t> big(hubproto::kMaxPayloadSize + 1, 0xAB);
  Header h = hello(0x03, 0x01);
  CHECK_FALSE(chip.publish(h, big.data(), big.size()));
}

TEST_CASE("a real-sized Thread dataset crosses the bus chip -> brain") {
  // The co-processor lends its Thread network to the brain (docs/matter-backlog.md
  // §3): the dataset is 109 bytes of TLVs and it travels in one frame. This is
  // the only frame in the system this big, so it gets its own test.
  InMemoryLink la, lb;
  InMemoryLink::connect(la, lb);
  UartTransport brain(la);
  UartTransport chip(lb);

  Capture at_brain;
  REQUIRE(brain.subscribe(&Capture::on_msg, &at_brain));

  std::vector<std::uint8_t> tlvs(109);
  for (std::size_t i = 0; i < tlvs.size(); ++i) {
    tlvs[i] = static_cast<std::uint8_t>((i * 37u + 11u) & 0xFFu);
  }
  std::vector<std::uint8_t> payload(1 + tlvs.size());
  auto enc = hubproto::encode_thread_dataset(tlvs.data(), tlvs.size(), payload.data(),
                                              payload.size());
  REQUIRE(enc.ok);

  Header h = hello(0x02, 0x04);
  h.type = MsgType::ThreadDataset;
  h.seq = 12;
  REQUIRE(chip.publish(h, payload.data(), enc.len));

  REQUIRE(at_brain.count == 1);
  CHECK(at_brain.last.type == MsgType::ThreadDataset);
  CHECK(at_brain.last.chip_id == 0x02);
  CHECK(at_brain.last.seq == 12);
  REQUIRE(at_brain.payload.size() == enc.len);

  hubproto::ThreadDatasetView view{};
  REQUIRE(hubproto::decode_thread_dataset(at_brain.payload.data(), at_brain.payload.size(), view));
  CHECK(view.len == tlvs.size());
  for (std::size_t i = 0; i < tlvs.size(); ++i) CHECK(view.bytes[i] == tlvs[i]);

  // The brain's link sees it as one clean frame, not as a broken one.
  CHECK(brain.bad_frames() == 0);
  CHECK(chip.tx_frames() == 1);
  CHECK(brain.rx_frames() == 1);
}
