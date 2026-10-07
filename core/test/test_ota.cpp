#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <vector>

#include "core/in_memory_link.hpp"
#include "core/ota_receiver.hpp"
#include "core/ota_sender.hpp"
#include "core/sha256.hpp"
#include "core/uart_transport.hpp"
#include "hubproto/message.hpp"
#include "hubproto/payload.hpp"
#include "hubproto/version.hpp"

using core::InMemoryLink;
using core::OtaReceiver;
using core::OtaSender;
using core::OtaSink;
using core::UartTransport;
using hubproto::Header;
using hubproto::MsgType;
using hubproto::OtaStatus;

namespace {

constexpr std::uint8_t kChip = 0x01;
constexpr std::uint8_t kChipBootId = 7;

// RAM-backed sink with switches to inject failures.
struct RamSink final : OtaSink {
  std::vector<std::uint8_t> data;
  std::uint32_t capacity = 1u << 20;
  bool fail_write = false;
  OtaStatus finish_status = OtaStatus::Ok;
  int begins = 0, aborts = 0, finishes = 0;

  OtaStatus begin(std::uint32_t size) noexcept override {
    ++begins;
    if (size > capacity) return OtaStatus::TooLarge;
    data.clear();
    return OtaStatus::Ok;
  }
  bool write(const std::uint8_t* d, std::size_t n) noexcept override {
    if (fail_write) return false;
    data.insert(data.end(), d, d + n);
    return true;
  }
  OtaStatus finish() noexcept override { ++finishes; return finish_status; }
  void abort() noexcept override { ++aborts; data.clear(); }
};

std::vector<std::uint8_t> make_image(std::size_t n) {
  std::vector<std::uint8_t> v(n);
  std::uint32_t x = 12345;
  for (auto& b : v) { x = x * 1664525u + 1013904223u; b = static_cast<std::uint8_t>(x >> 24); }
  return v;
}

struct Digest {
  std::uint8_t d[32];
};
Digest sha_of(const std::vector<std::uint8_t>& v) {
  Digest out{};
  core::Sha256 h;
  h.update(v.data(), v.size());
  h.finish(out.d);
  return out;
}

// Image source feeding the sender from a vector.
struct Source {
  const std::vector<std::uint8_t>* img;
  std::size_t pos = 0;
  static std::size_t read(void* ctx, std::uint8_t* buf, std::size_t cap) {
    auto* s = static_cast<Source*>(ctx);
    const std::size_t n = std::min(cap, s->img->size() - s->pos);
    for (std::size_t i = 0; i < n; ++i) buf[i] = (*s->img)[s->pos + i];
    s->pos += n;
    return n;
  }
};

// brain <-> chip over a loopback; the chip runs an OtaReceiver.
struct Rig {
  InMemoryLink la, lb;
  UartTransport brain, chip;
  RamSink sink;
  OtaReceiver rx;
  OtaSender tx;
  std::uint32_t now = 0;

  static OtaReceiver::Config rx_cfg() {
    OtaReceiver::Config c;
    c.own_chip_id = kChip;
    c.boot_id = kChipBootId;
    return c;
  }
  static OtaSender::Config tx_cfg() {
    OtaSender::Config c;
    c.ack_timeout_ms = 100;
    c.max_retries = 3;
    return c;
  }
  static InMemoryLink& wire(InMemoryLink& a, InMemoryLink& b) {
    InMemoryLink::connect(a, b);
    return a;
  }
  Rig() : brain(wire(la, lb)), chip(lb), rx(chip, sink, rx_cfg()), tx(brain, tx_cfg()) {}

  // Pump the sender until it finishes (ticks of 1 ms).
  void run(int max_ticks = 200000) {
    for (int i = 0; i < max_ticks && !tx.finished(); ++i) {
      tx.poll(now);
      rx.poll(now);
      ++now;
    }
  }
};

}  // namespace

TEST_CASE("an image transfers end to end and the chip asks for a reboot") {
  Rig r;
  const auto img = make_image(1000);  // not a multiple of the chunk size
  const auto d = sha_of(img);
  Source src{&img};
  REQUIRE(r.tx.start(kChip, static_cast<std::uint32_t>(img.size()), d.d, &Source::read, &src, 0));
  r.run();

  CHECK(r.tx.state() == OtaSender::State::Done);
  CHECK(r.tx.acked() == img.size());
  CHECK(r.sink.data == img);
  CHECK(r.sink.finishes == 1);
  CHECK(r.rx.reboot_pending());
  CHECK_FALSE(r.rx.active());
}

TEST_CASE("an exact multiple of the chunk size transfers too") {
  Rig r;
  const auto img = make_image(hubproto::kOtaMaxChunk * 3);
  const auto d = sha_of(img);
  Source src{&img};
  REQUIRE(r.tx.start(kChip, static_cast<std::uint32_t>(img.size()), d.d, &Source::read, &src, 0));
  r.run();
  CHECK(r.tx.state() == OtaSender::State::Done);
  CHECK(r.sink.data == img);
}

TEST_CASE("a wrong SHA-256 is rejected at END and the slot is dropped") {
  Rig r;
  const auto img = make_image(500);
  Digest bad = sha_of(img);
  bad.d[0] ^= 0xFF;
  Source src{&img};
  REQUIRE(r.tx.start(kChip, static_cast<std::uint32_t>(img.size()), bad.d, &Source::read, &src, 0));
  r.run();

  CHECK(r.tx.state() == OtaSender::State::Failed);
  CHECK(r.tx.failure() == OtaSender::Failure::Refused);
  CHECK(r.tx.status() == OtaStatus::VerifyFail);
  CHECK(r.sink.finishes == 0);
  CHECK(r.sink.aborts == 1);
  CHECK_FALSE(r.rx.reboot_pending());
}

TEST_CASE("an image too big for the slot is refused at BEGIN") {
  Rig r;
  r.sink.capacity = 100;
  const auto img = make_image(500);
  const auto d = sha_of(img);
  Source src{&img};
  REQUIRE(r.tx.start(kChip, static_cast<std::uint32_t>(img.size()), d.d, &Source::read, &src, 0));
  r.run();
  CHECK(r.tx.failure() == OtaSender::Failure::Refused);
  CHECK(r.tx.status() == OtaStatus::TooLarge);
  CHECK(r.sink.data.empty());
}

TEST_CASE("a flash write failure fails the transfer") {
  Rig r;
  r.sink.fail_write = true;
  const auto img = make_image(500);
  const auto d = sha_of(img);
  Source src{&img};
  REQUIRE(r.tx.start(kChip, static_cast<std::uint32_t>(img.size()), d.d, &Source::read, &src, 0));
  r.run();
  CHECK(r.tx.failure() == OtaSender::Failure::Refused);
  CHECK(r.tx.status() == OtaStatus::WriteFail);
  CHECK_FALSE(r.rx.active());
}

TEST_CASE("an image the sink cannot validate is refused and not activated") {
  Rig r;
  r.sink.finish_status = OtaStatus::VerifyFail;
  const auto img = make_image(300);
  const auto d = sha_of(img);
  Source src{&img};
  REQUIRE(r.tx.start(kChip, static_cast<std::uint32_t>(img.size()), d.d, &Source::read, &src, 0));
  r.run();
  CHECK(r.tx.status() == OtaStatus::VerifyFail);
  CHECK_FALSE(r.rx.reboot_pending());
}

TEST_CASE("a source that runs dry fails with SourceError") {
  Rig r;
  const auto img = make_image(300);
  const auto d = sha_of(img);
  Source src{&img};
  // Announce more bytes than the source has.
  REQUIRE(r.tx.start(kChip, 1000, d.d, &Source::read, &src, 0));
  r.run();
  CHECK(r.tx.failure() == OtaSender::Failure::SourceError);
}

TEST_CASE("every chip reply carries the chip's own chip_id and boot_id") {
  // RadioCoordinator resyncs on any boot_id change, so a wrong one here would
  // wipe the chip's entities from the registry mid-update.
  Rig r;
  struct Seen {
    std::vector<Header> hs;
    static void on(void* c, const Header& h, const std::uint8_t*, std::size_t) {
      static_cast<Seen*>(c)->hs.push_back(h);
    }
  } seen;
  r.brain.subscribe(&Seen::on, &seen);

  const auto img = make_image(500);
  const auto d = sha_of(img);
  Source src{&img};
  REQUIRE(r.tx.start(kChip, static_cast<std::uint32_t>(img.size()), d.d, &Source::read, &src, 0));
  r.run();

  REQUIRE(seen.hs.size() >= 4);  // begin ack, chunk acks, end ack
  for (const auto& h : seen.hs) {
    CHECK(h.chip_id == kChip);
    CHECK(h.boot_id == kChipBootId);
  }
}

TEST_CASE("a lost chunk ack is recovered by a resend without writing flash twice") {
  // Emulate the loss by replaying a chunk the chip already has: the receiver
  // must confirm it without appending it again.
  InMemoryLink la, lb;
  InMemoryLink::connect(la, lb);
  UartTransport brain(la), chip(lb);
  RamSink sink;
  OtaReceiver rx(chip, sink, Rig::rx_cfg());

  struct Acks {
    std::vector<hubproto::OtaChunkAck> v;
    static void on(void* c, const Header& h, const std::uint8_t* p, std::size_t n) {
      if (h.type != MsgType::OtaChunkAck) return;
      hubproto::OtaChunkAck a{};
      if (hubproto::decode_ota_chunk_ack(p, n, a)) static_cast<Acks*>(c)->v.push_back(a);
    }
  } acks;
  brain.subscribe(&Acks::on, &acks);

  const auto img = make_image(400);
  const auto d = sha_of(img);
  auto send = [&](MsgType t, std::uint16_t seq, const std::uint8_t* p, std::size_t n) {
    Header h{};
    h.version = hubproto::kProtocolVersion;
    h.type = t;
    h.chip_id = kChip;
    h.seq = seq;
    brain.publish(h, p, n);
  };

  std::uint8_t buf[255];
  auto b = hubproto::encode_ota_begin({400, {}}, buf, sizeof(buf));
  for (int i = 0; i < 32; ++i) buf[4 + i] = d.d[i];
  send(MsgType::OtaBegin, 1, buf, b.len);

  auto c0 = hubproto::encode_ota_chunk(0, img.data(), 200, buf, sizeof(buf));
  send(MsgType::OtaChunk, 2, buf, c0.len);
  send(MsgType::OtaChunk, 3, buf, c0.len);  // the resend of the same chunk

  REQUIRE(acks.v.size() == 2);
  CHECK(acks.v[0].status == OtaStatus::Ok);
  CHECK(acks.v[1].status == OtaStatus::Ok);
  CHECK(acks.v[1].next_offset == 200);
  CHECK(sink.data.size() == 200);  // written once
}

TEST_CASE("a chunk that skips ahead is answered with BadOffset and the wanted offset") {
  InMemoryLink la, lb;
  InMemoryLink::connect(la, lb);
  UartTransport brain(la), chip(lb);
  RamSink sink;
  OtaReceiver rx(chip, sink, Rig::rx_cfg());
  struct Acks {
    std::vector<hubproto::OtaChunkAck> v;
    static void on(void* c, const Header& h, const std::uint8_t* p, std::size_t n) {
      hubproto::OtaChunkAck a{};
      if (h.type == MsgType::OtaChunkAck && hubproto::decode_ota_chunk_ack(p, n, a))
        static_cast<Acks*>(c)->v.push_back(a);
    }
  } acks;
  brain.subscribe(&Acks::on, &acks);

  auto send = [&](MsgType t, std::uint16_t seq, const std::uint8_t* p, std::size_t n) {
    Header h{};
    h.version = hubproto::kProtocolVersion;
    h.type = t;
    h.chip_id = kChip;
    h.seq = seq;
    brain.publish(h, p, n);
  };
  std::uint8_t buf[255] = {};
  auto b = hubproto::encode_ota_begin({1000, {}}, buf, sizeof(buf));
  send(MsgType::OtaBegin, 1, buf, b.len);
  const std::uint8_t data[10] = {};
  auto c = hubproto::encode_ota_chunk(500, data, sizeof(data), buf, sizeof(buf));
  send(MsgType::OtaChunk, 2, buf, c.len);

  REQUIRE(acks.v.size() == 1);
  CHECK(acks.v[0].status == OtaStatus::BadOffset);
  CHECK(acks.v[0].next_offset == 0);
  CHECK(sink.data.empty());
}

TEST_CASE("a CHUNK without a BEGIN is refused") {
  InMemoryLink la, lb;
  InMemoryLink::connect(la, lb);
  UartTransport brain(la), chip(lb);
  RamSink sink;
  OtaReceiver rx(chip, sink, Rig::rx_cfg());
  struct Acks {
    std::vector<hubproto::OtaChunkAck> v;
    static void on(void* c, const Header& h, const std::uint8_t* p, std::size_t n) {
      hubproto::OtaChunkAck a{};
      if (h.type == MsgType::OtaChunkAck && hubproto::decode_ota_chunk_ack(p, n, a))
        static_cast<Acks*>(c)->v.push_back(a);
    }
  } acks;
  brain.subscribe(&Acks::on, &acks);

  std::uint8_t buf[255];
  const std::uint8_t data[4] = {1, 2, 3, 4};
  auto c = hubproto::encode_ota_chunk(0, data, sizeof(data), buf, sizeof(buf));
  Header h{};
  h.version = hubproto::kProtocolVersion;
  h.type = MsgType::OtaChunk;
  h.chip_id = kChip;
  h.seq = 1;
  brain.publish(h, buf, c.len);

  REQUIRE(acks.v.size() == 1);
  CHECK(acks.v[0].status == OtaStatus::BadState);
  CHECK(sink.data.empty());
}

TEST_CASE("a stalled transfer is aborted by the chip's idle timeout") {
  Rig r;
  const auto img = make_image(500);
  const auto d = sha_of(img);
  Source src{&img};
  REQUIRE(r.tx.start(kChip, static_cast<std::uint32_t>(img.size()), d.d, &Source::read, &src, 0));
  r.rx.poll(0);
  REQUIRE(r.rx.active());  // BEGIN accepted, the brain then goes silent
  r.rx.poll(14999);
  CHECK(r.rx.active());
  r.rx.poll(15000);
  CHECK_FALSE(r.rx.active());
  CHECK(r.sink.aborts == 1);
}

TEST_CASE("a restarted BEGIN replaces a transfer in progress") {
  Rig r;
  const auto img = make_image(500);
  const auto d = sha_of(img);
  Source src{&img};
  REQUIRE(r.tx.start(kChip, 500, d.d, &Source::read, &src, 0));
  r.tx.poll(0);  // first chunk goes out
  REQUIRE(r.rx.active());
  CHECK(r.rx.received() == 240);

  // The brain "reboots" and starts over with a fresh sender.
  OtaSender tx2(r.brain, Rig::tx_cfg());
  Source src2{&img};
  REQUIRE(tx2.start(kChip, 500, d.d, &Source::read, &src2, 0));
  CHECK(r.rx.received() == 0);
  CHECK(r.sink.aborts == 1);
  CHECK(r.sink.begins == 2);
}

TEST_CASE("a BEGIN after the image was accepted is refused as Busy") {
  Rig r;
  const auto img = make_image(100);
  const auto d = sha_of(img);
  Source src{&img};
  REQUIRE(r.tx.start(kChip, 100, d.d, &Source::read, &src, 0));
  r.run();
  REQUIRE(r.rx.reboot_pending());

  OtaSender tx2(r.brain, Rig::tx_cfg());
  Source src2{&img};
  REQUIRE(tx2.start(kChip, 100, d.d, &Source::read, &src2, 0));
  CHECK(tx2.failure() == OtaSender::Failure::Refused);
  CHECK(tx2.status() == OtaStatus::Busy);
}

TEST_CASE("a silent chip makes the sender time out after its retries") {
  InMemoryLink la, lb;
  InMemoryLink::connect(la, lb);
  UartTransport brain(la);  // no chip on the other end
  OtaSender tx(brain, Rig::tx_cfg());
  const auto img = make_image(100);
  const auto d = sha_of(img);
  Source src{&img};
  REQUIRE(tx.start(kChip, 100, d.d, &Source::read, &src, 0));

  // BEGIN timeout is 30 s by default; one send + 3 retries.
  std::uint32_t t = 0;
  for (int i = 0; i < 10 && !tx.finished(); ++i) {
    t += 30000;
    tx.poll(t);
  }
  CHECK(tx.state() == OtaSender::State::Failed);
  CHECK(tx.failure() == OtaSender::Failure::Timeout);
}

TEST_CASE("start() refuses a second transfer while one is running, and an empty image") {
  Rig r;
  const auto img = make_image(500);
  const auto d = sha_of(img);
  Source src{&img};
  CHECK_FALSE(r.tx.start(kChip, 0, d.d, &Source::read, &src, 0));
  REQUIRE(r.tx.start(kChip, 500, d.d, &Source::read, &src, 0));
  CHECK_FALSE(r.tx.start(kChip, 500, d.d, &Source::read, &src, 0));
}

TEST_CASE("abort() stops the sender and tells the chip to drop the slot") {
  Rig r;
  const auto img = make_image(500);
  const auto d = sha_of(img);
  Source src{&img};
  REQUIRE(r.tx.start(kChip, 500, d.d, &Source::read, &src, 0));
  r.tx.poll(0);
  REQUIRE(r.rx.active());
  r.tx.abort();
  CHECK(r.tx.failure() == OtaSender::Failure::Aborted);
  CHECK_FALSE(r.rx.active());
  CHECK(r.sink.aborts == 1);
}

namespace {

// Chip-side link that loses the n-th frame it writes (1-based), else forwards.
struct DropNthLink final : core::ByteLink {
  core::ByteLink& inner;
  int drop_at;
  int writes = 0;
  DropNthLink(core::ByteLink& l, int n) : inner(l), drop_at(n) {}
  bool write(const std::uint8_t* d, std::size_t n) noexcept override {
    if (++writes == drop_at) return true;  // lost on the wire
    return inner.write(d, n);
  }
  void set_receiver(ReceiveFn fn, void* ctx) noexcept override { inner.set_receiver(fn, ctx); }
};

// Drop chip write #n during a 600-byte transfer and check it still completes.
void run_with_lost_chip_frame(int n, std::uint32_t min_elapsed_ms) {
  InMemoryLink la, lb;
  InMemoryLink::connect(la, lb);
  DropNthLink lossy(lb, n);
  UartTransport brain(la), chip(lossy);
  RamSink sink;
  OtaReceiver rx(chip, sink, Rig::rx_cfg());
  OtaSender tx(brain, Rig::tx_cfg());

  const auto img = make_image(600);
  const auto d = sha_of(img);
  Source src{&img};
  REQUIRE(tx.start(kChip, static_cast<std::uint32_t>(img.size()), d.d, &Source::read, &src, 0));
  std::uint32_t now = 0;
  for (; now < 200000 && !tx.finished(); ++now) {
    tx.poll(now);
    rx.poll(now);
  }
  CHECK(tx.state() == OtaSender::State::Done);
  CHECK(sink.data == img);  // no chunk written twice, none missing
  CHECK(now >= min_elapsed_ms);  // it really did wait for a timeout
}

}  // namespace

TEST_CASE("a lost BEGIN ack is recovered by resending BEGIN") {
  // The resent BEGIN restarts the (idempotent) transfer on the chip.
  run_with_lost_chip_frame(1, 30000);
}

TEST_CASE("a lost chunk ack is recovered by the sender's resend") {
  run_with_lost_chip_frame(2, 100);
}

TEST_CASE("a lost END ack is recovered by resending END") {
  // 600 bytes = 3 chunks => chip writes: BEGIN ack, 3 chunk acks, END ack (#5).
  run_with_lost_chip_frame(5, 20000);
}

TEST_CASE("a resent END after the image was accepted is confirmed, not refused") {
  Rig r;
  const auto img = make_image(100);
  const auto d = sha_of(img);
  Source src{&img};
  REQUIRE(r.tx.start(kChip, 100, d.d, &Source::read, &src, 0));
  r.run();
  REQUIRE(r.rx.reboot_pending());

  struct Acks {
    std::vector<OtaStatus> v;
    static void on(void* c, const Header& h, const std::uint8_t* p, std::size_t n) {
      OtaStatus s{};
      if (h.type == MsgType::OtaEndAck && hubproto::decode_ota_end_ack(p, n, s))
        static_cast<Acks*>(c)->v.push_back(s);
    }
  } acks;
  r.brain.subscribe(&Acks::on, &acks);
  Header h{};
  h.version = hubproto::kProtocolVersion;
  h.type = MsgType::OtaEnd;
  h.chip_id = kChip;
  h.seq = 99;
  r.brain.publish(h, nullptr, 0);
  REQUIRE(acks.v.size() == 1);
  CHECK(acks.v[0] == OtaStatus::Ok);
  CHECK(r.sink.finishes == 1);  // validated once, not again
}
