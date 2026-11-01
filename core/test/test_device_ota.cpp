#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <vector>

#include "core/device_ota_receiver.hpp"
#include "core/device_ota_sender.hpp"
#include "core/in_memory_link.hpp"
#include "core/sha256.hpp"
#include "core/uart_transport.hpp"
#include "core/zigbee_ota.hpp"
#include "hubproto/message.hpp"
#include "hubproto/payload.hpp"
#include "hubproto/version.hpp"

using core::DeviceOtaReceiver;
using core::DeviceOtaSender;
using core::DeviceOtaSink;
using core::InMemoryLink;
using core::UartTransport;
using hubproto::DevOtaPhase;
using hubproto::DevOtaStatus;
using hubproto::MsgType;

namespace {

constexpr std::uint8_t kChip = 0x01;
constexpr std::uint8_t kChipBootId = 7;
constexpr std::uint16_t kDeviceEntity = 0x0100;  // the lamp/dimmer we update

// RAM-backed sink with switches to inject failures.
struct RamSink final : DeviceOtaSink {
  std::vector<std::uint8_t> data;
  std::uint32_t capacity = 1u << 20;
  bool fail_write = false;
  DevOtaStatus finish_status = DevOtaStatus::Ok;
  int begins = 0, aborts = 0, finishes = 0;
  std::uint16_t entity = 0;

  DevOtaStatus begin(std::uint16_t e, std::uint32_t size) noexcept override {
    ++begins;
    entity = e;
    if (size > capacity) return DevOtaStatus::TooLarge;
    data.clear();
    return DevOtaStatus::Ok;
  }
  bool write(const std::uint8_t* d, std::size_t n) noexcept override {
    if (fail_write) return false;
    data.insert(data.end(), d, d + n);
    return true;
  }
  DevOtaStatus finish() noexcept override { ++finishes; return finish_status; }
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

struct PhaseEvent {
  std::uint16_t entity;
  DevOtaPhase phase;
  DevOtaStatus status;
  std::uint8_t percent;
};

// brain <-> chip over a loopback; the chip runs a DeviceOtaReceiver.
struct Rig {
  InMemoryLink la, lb;
  UartTransport brain, chip;
  RamSink sink;
  DeviceOtaReceiver rx;
  DeviceOtaSender tx;
  std::uint32_t now = 0;
  std::vector<PhaseEvent> phases;

  static DeviceOtaReceiver::Config rx_cfg() {
    DeviceOtaReceiver::Config c;
    c.own_chip_id = kChip;
    c.boot_id = kChipBootId;
    return c;
  }
  static DeviceOtaSender::Config tx_cfg() {
    DeviceOtaSender::Config c;
    c.ack_timeout_ms = 100;
    c.max_retries = 3;
    return c;
  }
  static InMemoryLink& wire(InMemoryLink& a, InMemoryLink& b) {
    InMemoryLink::connect(a, b);
    return a;
  }
  Rig() : brain(wire(la, lb)), chip(lb), rx(chip, sink, rx_cfg()), tx(brain, tx_cfg()) {
    tx.on_phase(&Rig::on_phase, this);
  }

  static void on_phase(void* ctx, std::uint16_t entity, DevOtaPhase phase,
                       DevOtaStatus status, std::uint8_t percent) {
    auto* r = static_cast<Rig*>(ctx);
    r->phases.push_back({entity, phase, status, percent});
  }

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

TEST_CASE("a .ota file reaches the chip intact and the device is notified") {
  Rig r;
  const auto img = make_image(1000);  // not a multiple of the chunk size
  const auto d = sha_of(img);
  Source src{&img};
  REQUIRE(r.tx.start(kChip, kDeviceEntity, static_cast<std::uint32_t>(img.size()), d.d,
                     &Source::read, &src, 0));
  r.run();

  CHECK(r.tx.state() == DeviceOtaSender::State::Done);
  CHECK(r.tx.acked() == img.size());
  CHECK(r.sink.data == img);
  CHECK(r.sink.finishes == 1);
  CHECK(r.sink.entity == kDeviceEntity);
  CHECK(r.rx.accepted());
  CHECK_FALSE(r.rx.active());
  CHECK(r.rx.entity_id() == kDeviceEntity);
}

TEST_CASE("an exact multiple of the chunk size transfers too") {
  Rig r;
  const auto img = make_image(hubproto::kDevOtaMaxChunk * 3);
  const auto d = sha_of(img);
  Source src{&img};
  REQUIRE(r.tx.start(kChip, kDeviceEntity, static_cast<std::uint32_t>(img.size()), d.d,
                     &Source::read, &src, 0));
  r.run();
  CHECK(r.tx.state() == DeviceOtaSender::State::Done);
  CHECK(r.sink.data == img);
}

TEST_CASE("a wrong SHA-256 is rejected at END and the file is dropped") {
  Rig r;
  const auto img = make_image(500);
  Digest bad = sha_of(img);
  bad.d[0] ^= 0xFF;
  Source src{&img};
  REQUIRE(r.tx.start(kChip, kDeviceEntity, static_cast<std::uint32_t>(img.size()), bad.d,
                     &Source::read, &src, 0));
  r.run();

  CHECK(r.tx.state() == DeviceOtaSender::State::Failed);
  CHECK(r.tx.failure() == DeviceOtaSender::Failure::Refused);
  CHECK(r.tx.status() == DevOtaStatus::BadChecksum);
  CHECK(r.sink.finishes == 0);
  CHECK(r.sink.aborts == 1);
  CHECK_FALSE(r.rx.accepted());
}

TEST_CASE("a file too big for the chip is refused at BEGIN") {
  Rig r;
  r.sink.capacity = 100;
  const auto img = make_image(500);
  const auto d = sha_of(img);
  Source src{&img};
  REQUIRE(r.tx.start(kChip, kDeviceEntity, static_cast<std::uint32_t>(img.size()), d.d,
                     &Source::read, &src, 0));
  r.run();
  CHECK(r.tx.failure() == DeviceOtaSender::Failure::Refused);
  CHECK(r.tx.status() == DevOtaStatus::TooLarge);
  CHECK(r.sink.data.empty());
}

TEST_CASE("a flash write failure fails the transfer") {
  Rig r;
  r.sink.fail_write = true;
  const auto img = make_image(500);
  const auto d = sha_of(img);
  Source src{&img};
  REQUIRE(r.tx.start(kChip, kDeviceEntity, static_cast<std::uint32_t>(img.size()), d.d,
                     &Source::read, &src, 0));
  r.run();
  CHECK(r.tx.failure() == DeviceOtaSender::Failure::Refused);
  CHECK_FALSE(r.rx.active());
}

TEST_CASE("a file the chip cannot parse is refused and never served") {
  Rig r;
  r.sink.finish_status = DevOtaStatus::BadFormat;
  const auto img = make_image(300);
  const auto d = sha_of(img);
  Source src{&img};
  REQUIRE(r.tx.start(kChip, kDeviceEntity, static_cast<std::uint32_t>(img.size()), d.d,
                     &Source::read, &src, 0));
  r.run();
  CHECK(r.tx.failure() == DeviceOtaSender::Failure::Refused);
  CHECK(r.tx.status() == DevOtaStatus::BadFormat);
  CHECK(r.sink.finishes == 1);
  CHECK_FALSE(r.rx.accepted());
}

TEST_CASE("the device session is reported to the brain after the file is accepted") {
  Rig r;
  const auto img = make_image(400);
  const auto d = sha_of(img);
  Source src{&img};
  REQUIRE(r.tx.start(kChip, kDeviceEntity, static_cast<std::uint32_t>(img.size()), d.d,
                     &Source::read, &src, 0));
  r.run();
  REQUIRE(r.tx.state() == DeviceOtaSender::State::Done);

  // The chip now talks to the device: the sink pushes the session phases.
  r.rx.report_phase(DevOtaPhase::Notifying, DevOtaStatus::Ok, 0);
  r.rx.report_phase(DevOtaPhase::Transferring, DevOtaStatus::Ok, 42);
  r.rx.report_phase(DevOtaPhase::Done, DevOtaStatus::Ok, 100);

  REQUIRE(r.phases.size() == 3);
  CHECK(r.phases[0].entity == kDeviceEntity);
  CHECK(r.phases[0].phase == DevOtaPhase::Notifying);
  CHECK(r.phases[1].phase == DevOtaPhase::Transferring);
  CHECK(r.phases[1].percent == 42);
  CHECK(r.phases[2].phase == DevOtaPhase::Done);
  CHECK(r.tx.phase() == DevOtaPhase::Done);
  CHECK(r.tx.percent() == 100);
}

TEST_CASE("a failed device session is reported with its status") {
  Rig r;
  const auto img = make_image(400);
  const auto d = sha_of(img);
  Source src{&img};
  REQUIRE(r.tx.start(kChip, kDeviceEntity, static_cast<std::uint32_t>(img.size()), d.d,
                     &Source::read, &src, 0));
  r.run();
  REQUIRE(r.tx.state() == DeviceOtaSender::State::Done);

  r.rx.report_phase(DevOtaPhase::Failed, DevOtaStatus::Mismatch, 0);
  REQUIRE(r.phases.size() == 1);
  CHECK(r.phases[0].phase == DevOtaPhase::Failed);
  CHECK(r.phases[0].status == DevOtaStatus::Mismatch);
  CHECK(r.tx.status() == DevOtaStatus::Mismatch);
}

TEST_CASE("a stalled transfer is dropped by the chip") {
  Rig r;
  const auto img = make_image(500);
  const auto d = sha_of(img);
  Source src{&img};
  REQUIRE(r.tx.start(kChip, kDeviceEntity, static_cast<std::uint32_t>(img.size()), d.d,
                     &Source::read, &src, 0));
  // No polling at all: the receiver's idle timeout must fire.
  for (int i = 0; i < 20000; ++i) {
    r.rx.poll(r.now);
    ++r.now;
  }
  CHECK_FALSE(r.rx.active());
  CHECK(r.sink.aborts == 1);
}

// The sink the firmware uses: it checks the file the way the coordinator does
// (header + CRC32) before the image may be offered to a device.
struct ValidatingSink final : DeviceOtaSink {
  std::vector<std::uint8_t> data;
  bool header_ok = false;
  bool crc_ok = false;
  std::uint16_t manufacturer = 0;
  std::uint16_t image_type = 0;

  hubproto::DevOtaStatus begin(std::uint16_t, std::uint32_t) noexcept override {
    data.clear();
    return hubproto::DevOtaStatus::Ok;
  }
  bool write(const std::uint8_t* d, std::size_t n) noexcept override {
    data.insert(data.end(), d, d + n);
    return true;
  }
  hubproto::DevOtaStatus finish() noexcept override {
    core::zota::FileHeader h{};
    header_ok = core::zota::parse_header(data.data(), data.size(), h);
    if (header_ok) {
      manufacturer = h.manufacturer_code;
      image_type = h.image_type;
    }
    crc_ok = core::zota::verify_crc(data.data(), data.size());
    return (header_ok && crc_ok) ? hubproto::DevOtaStatus::Ok : hubproto::DevOtaStatus::BadFormat;
  }
  void abort() noexcept override { data.clear(); }
};

TEST_CASE("a real Zigbee OTA file travels to the chip and passes its checks") {
  // A vendor-like .ota file: 56-byte header + upgrade-image sub-element + CRC32.
  std::vector<std::uint8_t> payload = make_image(4096);
  std::vector<std::uint8_t> file(core::zota::kFileOverhead + payload.size());
  const std::size_t n = core::zota::build_file(file.data(), file.size(), payload.data(),
                                                payload.size(), 0x117C, 0x0100, 7, 0x0002);
  REQUIRE(n == file.size());

  ValidatingSink sink;
  InMemoryLink la, lb;
  InMemoryLink::connect(la, lb);
  UartTransport brain(la), chip(lb);
  core::DeviceOtaReceiver::Config rcfg{};
  rcfg.own_chip_id = kChip;
  rcfg.boot_id = kChipBootId;
  core::DeviceOtaReceiver rx(chip, sink, rcfg);
  core::DeviceOtaSender::Config scfg{};
  scfg.ack_timeout_ms = 100;
  core::DeviceOtaSender tx(brain, scfg);

  const auto d = sha_of(file);
  Source src{&file};
  REQUIRE(tx.start(kChip, kDeviceEntity, static_cast<std::uint32_t>(file.size()), d.d,
                   &Source::read, &src, 0));
  std::uint32_t now = 0;
  for (int i = 0; i < 200000 && !tx.finished(); ++i) {
    tx.poll(now);
    rx.poll(now);
    ++now;
  }
  CHECK(tx.state() == DeviceOtaSender::State::Done);
  CHECK(sink.data == file);
  CHECK(sink.header_ok);
  CHECK(sink.crc_ok);
  CHECK(sink.manufacturer == 0x117C);   // IKEA of Sweden
  CHECK(sink.image_type == 0x0100);
  CHECK(rx.accepted());
}

TEST_CASE("a corrupted .ota file is refused by the chip's checks") {
  std::vector<std::uint8_t> payload = make_image(512);
  std::vector<std::uint8_t> file(core::zota::kFileOverhead + payload.size());
  const std::size_t n = core::zota::build_file(file.data(), file.size(), payload.data(),
                                                payload.size(), 0x117C, 0x0100, 7, 0x0002);
  REQUIRE(n == file.size());
  file[100] ^= 0xFF;  // a flipped bit inside the firmware

  ValidatingSink sink;
  InMemoryLink la, lb;
  InMemoryLink::connect(la, lb);
  UartTransport brain(la), chip(lb);
  core::DeviceOtaReceiver::Config rcfg{};
  rcfg.own_chip_id = kChip;
  core::DeviceOtaReceiver rx(chip, sink, rcfg);
  core::DeviceOtaSender::Config scfg{};
  scfg.ack_timeout_ms = 100;
  core::DeviceOtaSender tx(brain, scfg);

  const auto d = sha_of(file);
  Source src{&file};
  REQUIRE(tx.start(kChip, kDeviceEntity, static_cast<std::uint32_t>(file.size()), d.d,
                   &Source::read, &src, 0));
  std::uint32_t now = 0;
  for (int i = 0; i < 200000 && !tx.finished(); ++i) {
    tx.poll(now);
    rx.poll(now);
    ++now;
  }
  CHECK(tx.state() == DeviceOtaSender::State::Failed);
  CHECK(tx.status() == hubproto::DevOtaStatus::BadFormat);
  CHECK(sink.crc_ok == false);
  CHECK_FALSE(rx.accepted());
}
