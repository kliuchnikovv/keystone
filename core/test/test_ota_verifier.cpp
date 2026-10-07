#include <doctest/doctest.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "core/in_memory_link.hpp"
#include "core/ota_receiver.hpp"
#include "core/ota_verifier.hpp"
#include "core/uart_transport.hpp"
#include "hubproto/message.hpp"
#include "hubproto/payload.hpp"
#include "hubproto/version.hpp"

using core::InMemoryLink;
using core::OtaVerifier;
using core::UartTransport;
using hubproto::Header;
using hubproto::MsgType;
using V = core::OtaVerifier::Verdict;

namespace {

constexpr std::uint8_t kChip = 0x01;

// A chip that answers OTA_INFO_REQ from a script. Can also stay silent.
struct Chip {
  UartTransport* bus = nullptr;
  bool answer = true;
  std::uint8_t boot_id = 5;
  hubproto::OtaInfo info{};
  int requests = 0;
  static void on(void* c, const Header& h, const std::uint8_t*, std::size_t) {
    auto* self = static_cast<Chip*>(c);
    if (h.type != MsgType::OtaInfoReq) return;
    ++self->requests;
    if (!self->answer) return;
    std::uint8_t buf[hubproto::kOtaInfoSize];
    const auto r = hubproto::encode_ota_info(self->info, buf, sizeof(buf));
    Header rsp{};
    rsp.version = hubproto::kProtocolVersion;
    rsp.type = MsgType::OtaInfo;
    rsp.chip_id = kChip;
    rsp.boot_id = self->boot_id;
    rsp.seq = h.seq;
    self->bus->publish(rsp, buf, r.len);
  }
};

struct Done {
  std::vector<V> v;
  static void on(void* c, std::uint8_t, V verdict) { static_cast<Done*>(c)->v.push_back(verdict); }
};

struct Rig {
  InMemoryLink la, lb;
  UartTransport brain{la}, chipbus{lb};
  Chip chip;
  OtaVerifier ver{brain};
  Done done;
  std::uint8_t sha[32];
  Rig() {
    InMemoryLink::connect(la, lb);
    chip.bus = &chipbus;
    chipbus.subscribe(&Chip::on, &chip);
    ver.on_done(&Done::on, &done);
    for (int i = 0; i < 32; ++i) sha[i] = static_cast<std::uint8_t>(i + 1);
    std::memcpy(chip.info.last_ota_sha256, sha, 32);
    chip.info.last_ota_slot = 1;
  }
};

}  // namespace

TEST_CASE("new image running and confirmed -> Verified") {
  Rig r;
  r.chip.boot_id = 6;  // it rebooted (was 5)
  r.chip.info.running_slot = 1;
  r.chip.info.confirmed = true;
  r.ver.start(kChip, r.sha, 5, 0);
  r.ver.poll(0);
  CHECK(r.ver.verdict() == V::Verified);
  REQUIRE(r.done.v.size() == 1);
  CHECK(r.done.v[0] == V::Verified);
}

TEST_CASE("chip runs the old slot after the reboot -> RolledBack") {
  Rig r;
  r.chip.boot_id = 7;
  r.chip.info.running_slot = 0;  // sent to slot 1, but slot 0 is running
  r.chip.info.confirmed = true;
  r.ver.start(kChip, r.sha, 5, 0);
  r.ver.poll(0);
  CHECK(r.ver.verdict() == V::RolledBack);
}

TEST_CASE("a chip with no OTA record cannot be judged -> NoRecord, not RolledBack") {
  Rig r;
  r.chip.boot_id = 6;
  r.chip.info.running_slot = 0;
  r.chip.info.confirmed = true;
  r.chip.info.last_ota_slot = hubproto::kOtaSlotUnknown;
  std::memset(r.chip.info.last_ota_sha256, 0, 32);
  r.ver.start(kChip, r.sha, 5, 0);
  r.ver.poll(0);
  CHECK(r.ver.verdict() == V::NoRecord);
}

TEST_CASE("a different last-OTA sha is also a rollback") {
  Rig r;
  r.chip.boot_id = 6;
  r.chip.info.running_slot = 1;
  r.chip.info.confirmed = true;
  r.chip.info.last_ota_sha256[0] ^= 0xFF;  // an older image, not the one we sent
  r.ver.start(kChip, r.sha, 5, 0);
  r.ver.poll(0);
  CHECK(r.ver.verdict() == V::RolledBack);
}

TEST_CASE("an answer from before the reboot (same boot_id) is ignored") {
  Rig r;
  r.chip.boot_id = 5;            // still the old run
  r.chip.info.running_slot = 0;  // old image: would look like a rollback
  r.ver.start(kChip, r.sha, 5, 0);
  r.ver.poll(0);
  CHECK(r.ver.waiting());
  // Now it rebooted into the new image.
  r.chip.boot_id = 6;
  r.chip.info.running_slot = 1;
  r.chip.info.confirmed = true;
  r.ver.poll(5000);
  CHECK(r.ver.verdict() == V::Verified);
}

TEST_CASE("pending-verify image keeps us asking, then Verified once confirmed") {
  Rig r;
  r.chip.boot_id = 6;
  r.chip.info.running_slot = 1;
  r.chip.info.confirmed = false;
  r.ver.start(kChip, r.sha, 5, 0);
  r.ver.poll(0);
  CHECK(r.ver.waiting());
  r.chip.info.confirmed = true;  // the 30 s grace passed
  r.ver.poll(5000);
  CHECK(r.ver.verdict() == V::Verified);
}

TEST_CASE("still unconfirmed at the deadline -> RunningUnconfirmed; silence -> NoAnswer") {
  {
    Rig r;
    r.chip.boot_id = 6;
    r.chip.info.running_slot = 1;
    r.chip.info.confirmed = false;
    r.ver.start(kChip, r.sha, 5, 0);
    for (std::uint32_t t = 0; t <= 120000; t += 5000) r.ver.poll(t);
    CHECK(r.ver.verdict() == V::RunningUnconfirmed);
  }
  {
    Rig r;
    r.chip.answer = false;
    r.ver.start(kChip, r.sha, 5, 0);
    for (std::uint32_t t = 0; t <= 120000; t += 5000) r.ver.poll(t);
    CHECK(r.ver.verdict() == V::NoAnswer);
    CHECK(r.chip.requests >= 20);
  }
}

TEST_CASE("requests are paced, not sent on every poll") {
  Rig r;
  r.chip.answer = false;
  r.ver.start(kChip, r.sha, 5, 0);
  r.ver.poll(0);
  r.ver.poll(100);
  r.ver.poll(4999);
  CHECK(r.chip.requests == 1);
  r.ver.poll(5000);
  CHECK(r.chip.requests == 2);
}

namespace {
struct InfoSink final : core::OtaSink {
  std::uint8_t recorded[32] = {};
  bool was_recorded = false;
  hubproto::OtaStatus begin(std::uint32_t) noexcept override { return hubproto::OtaStatus::Ok; }
  bool write(const std::uint8_t*, std::size_t) noexcept override { return true; }
  hubproto::OtaStatus finish() noexcept override { return hubproto::OtaStatus::Ok; }
  void abort() noexcept override {}
  void record_image(const std::uint8_t* sha) noexcept override {
    std::memcpy(recorded, sha, 32);
    was_recorded = true;
  }
  bool info(hubproto::OtaInfo& out) const noexcept override {
    out.running_slot = 0;
    out.confirmed = true;
    out.last_ota_slot = 1;
    std::memcpy(out.last_ota_sha256, recorded, 32);
    return true;
  }
};
}  // namespace

TEST_CASE("OtaReceiver answers OTA_INFO_REQ from its sink (and only if the sink can)") {
  InMemoryLink la, lb;
  InMemoryLink::connect(la, lb);
  UartTransport brain(la), chip(lb);
  InfoSink sink;
  core::OtaReceiver::Config c;
  c.own_chip_id = kChip;
  c.boot_id = 9;
  core::OtaReceiver rx(chip, sink, c);

  std::vector<hubproto::OtaInfo> got;
  std::vector<std::uint8_t> boot_ids;
  struct Ctx { std::vector<hubproto::OtaInfo>* v; std::vector<std::uint8_t>* b; } ctx{&got, &boot_ids};
  brain.subscribe([](void* p, const Header& h, const std::uint8_t* d, std::size_t n) {
    if (h.type != MsgType::OtaInfo) return;
    auto* x = static_cast<Ctx*>(p);
    hubproto::OtaInfo i{};
    REQUIRE(hubproto::decode_ota_info(d, n, i));
    x->v->push_back(i);
    x->b->push_back(h.boot_id);
  }, &ctx);

  Header q{};
  q.version = hubproto::kProtocolVersion;
  q.type = MsgType::OtaInfoReq;
  q.chip_id = kChip;
  brain.publish(q, nullptr, 0);
  REQUIRE(got.size() == 1);
  CHECK(boot_ids[0] == 9);  // carries the chip's real boot_id
  CHECK(got[0].running_slot == 0);
  CHECK(got[0].last_ota_slot == 1);
}
