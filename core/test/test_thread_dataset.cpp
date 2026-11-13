#include <doctest/doctest.h>

#include <cstring>
#include <string>
#include <vector>

#include "core/thread_dataset.hpp"

namespace {

// A dataset as OpenThread writes it and CHIP reads it: the MeshCoP TLV
// numbering, and a channel TLV of three bytes — page, then the channel number
// in two bytes, big endian.
std::vector<std::uint8_t> good_dataset() {
  std::vector<std::uint8_t> d;
  auto tlv = [&d](std::uint8_t type, const std::vector<std::uint8_t>& value) {
    d.push_back(type);
    d.push_back(static_cast<std::uint8_t>(value.size()));
    d.insert(d.end(), value.begin(), value.end());
  };
  tlv(core::kTlvActiveTimestamp, {0, 0, 0, 0, 0, 0, 0, 1});
  tlv(core::kTlvChannel, {0x00, 0x00, 15});
  tlv(core::kTlvPanId, {0x12, 0x34});
  tlv(core::kTlvExtPanId, {1, 2, 3, 4, 5, 6, 7, 8});
  tlv(core::kTlvMeshLocalPrefix, {0xfd, 0, 0, 0, 0, 0, 0, 0});
  tlv(core::kTlvMasterKey, {0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9});
  tlv(core::kTlvSecurityPolicy, {0, 0, 0});
  tlv(core::kTlvNetworkName, {'O', 'p', 'e', 'n', 'T', 'h', 'r', 'e', 'a', 'd'});
  return d;
}

}  // namespace

TEST_CASE("the dataset: the TLV numbers are the ones OpenThread and CHIP use") {
  // Both sides have to agree byte for byte, because these are the bytes the
  // device is handed over BLE. OpenThread: OT_MESHCOP_TLV_* in dataset.h.
  // CHIP: chip::Thread::ThreadTLV in ThreadOperationalDataset.cpp.
  CHECK(core::kTlvChannel == 0);
  CHECK(core::kTlvPanId == 1);
  CHECK(core::kTlvExtPanId == 2);
  CHECK(core::kTlvNetworkName == 3);
  CHECK(core::kTlvPskc == 4);
  CHECK(core::kTlvMasterKey == 5);
  CHECK(core::kTlvMeshLocalPrefix == 7);
  CHECK(core::kTlvSecurityPolicy == 12);
  CHECK(core::kTlvActiveTimestamp == 14);
}

TEST_CASE("the dataset: a well-formed one is accepted, with the channel and the name read out") {
  const auto bytes = good_dataset();
  core::ThreadDataset ds;
  REQUIRE(ds.set(bytes.data(), bytes.size()));
  CHECK(ds.valid());
  CHECK(ds.size() == bytes.size());
  CHECK(ds.channel() == 15);
  CHECK(std::string(ds.network_name()) == "OpenThread");
  CHECK(std::string(ds.ext_pan_id()) == "0102030405060708");
  CHECK(ds.has(core::kTlvMasterKey));
  CHECK(ds.has(core::kTlvExtPanId));
  CHECK_FALSE(ds.has(0x42));
  CHECK(std::memcmp(ds.data(), bytes.data(), bytes.size()) == 0);
}

TEST_CASE("the dataset: the channel is the number, not the page") {
  // The channel TLV is page (one byte, 0) + number (two bytes, big endian).
  // Reading the page instead gives 0 for every network — and a network on
  // channel 0 does not exist, which is a quiet way to be wrong.
  std::vector<std::uint8_t> d;
  auto tlv = [&d](std::uint8_t type, const std::vector<std::uint8_t>& value) {
    d.push_back(type);
    d.push_back(static_cast<std::uint8_t>(value.size()));
    d.insert(d.end(), value.begin(), value.end());
  };
  tlv(core::kTlvChannel, {0x00, 0x00, 26});
  tlv(core::kTlvPanId, {0x12, 0x34});
  tlv(core::kTlvMasterKey, {0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9});

  core::ThreadDataset ds;
  REQUIRE(ds.set(d.data(), d.size()));
  CHECK(ds.channel() == 26);
}

TEST_CASE("the dataset: an empty or oversized one is refused") {
  const auto bytes = good_dataset();
  core::ThreadDataset ds;
  CHECK_FALSE(ds.set(nullptr, 0));
  CHECK_FALSE(ds.set(bytes.data(), 0));
  CHECK_FALSE(ds.valid());
  std::vector<std::uint8_t> huge(core::ThreadDataset::kMaxBytes + 1, 0);
  CHECK_FALSE(ds.set(huge.data(), huge.size()));
  CHECK_FALSE(ds.valid());
}

TEST_CASE("the dataset: broken TLVs are refused") {
  core::ThreadDataset ds;

  // A value that runs past the end of the buffer: the PAN id TLV claims two
  // bytes but only one is there.
  const std::uint8_t over[] = {0x01, 0x02, 0x12};
  CHECK_FALSE(ds.set(over, sizeof(over)));

  // A header cut in half: a type with no length.
  const std::uint8_t half_header[] = {0x00, 0x03, 15, 0x00, 0x00, 0x01, 0x02, 0x12, 0x34, 0x0E};
  CHECK_FALSE(ds.set(half_header, sizeof(half_header)));

  // Garbage that does not parse as TLVs at all.
  const std::uint8_t junk[] = {0xFF, 0xFF, 0xFF, 0xFF};
  CHECK_FALSE(ds.set(junk, sizeof(junk)));
}

TEST_CASE("the dataset: one without a channel, a PAN id or a network key is refused") {
  core::ThreadDataset ds;

  auto without = [&](std::uint8_t type) {
    const auto bytes = good_dataset();
    std::vector<std::uint8_t> out;
    std::size_t i = 0;
    while (i + 2 <= bytes.size()) {
      const std::uint8_t t = bytes[i];
      const std::size_t vlen = bytes[i + 1];
      if (t != type) out.insert(out.end(), bytes.begin() + static_cast<long>(i),
                                bytes.begin() + static_cast<long>(i + 2 + vlen));
      i += 2 + vlen;
    }
    return out;
  };

  CHECK_FALSE(ds.set(without(core::kTlvChannel).data(), without(core::kTlvChannel).size()));
  CHECK_FALSE(ds.set(without(core::kTlvPanId).data(), without(core::kTlvPanId).size()));
  CHECK_FALSE(ds.set(without(core::kTlvMasterKey).data(), without(core::kTlvMasterKey).size()));
}

TEST_CASE("the dataset: clear() forgets it") {
  const auto bytes = good_dataset();
  core::ThreadDataset ds;
  REQUIRE(ds.set(bytes.data(), bytes.size()));
  ds.clear();
  CHECK_FALSE(ds.valid());
  CHECK(ds.size() == 0);
  CHECK(ds.channel() == 0);
  CHECK(std::string(ds.network_name()).empty());
  CHECK(std::string(ds.ext_pan_id()).empty());
}

TEST_CASE("the dataset: the extended PAN id tells which network this is") {
  // The network identity, as hex. It is what says whether the devices that
  // were commissioned earlier still live in the network the brain is about to
  // hand to the next one: a different ext PAN id means a different network,
  // and the old devices are orphaned.
  std::vector<std::uint8_t> d;
  auto tlv = [&d](std::uint8_t type, const std::vector<std::uint8_t>& value) {
    d.push_back(type);
    d.push_back(static_cast<std::uint8_t>(value.size()));
    d.insert(d.end(), value.begin(), value.end());
  };
  tlv(core::kTlvChannel, {0x00, 0x00, 26});
  tlv(core::kTlvPanId, {0x12, 0x34});
  tlv(core::kTlvMasterKey, {0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9});
  tlv(core::kTlvExtPanId, {0xe2, 0x2c, 0x46, 0x67, 0x86, 0x47, 0xf5, 0x40});

  core::ThreadDataset ds;
  REQUIRE(ds.set(d.data(), d.size()));
  CHECK(std::string(ds.ext_pan_id()) == "e22c46678647f540");

  // A dataset without that TLV has no identity to report.
  std::vector<std::uint8_t> without;
  std::size_t i = 0;
  while (i + 2 <= d.size()) {
    const std::uint8_t t = d[i];
    const std::size_t vlen = d[i + 1];
    if (t != core::kTlvExtPanId)
      without.insert(without.end(), d.begin() + static_cast<long>(i),
                     d.begin() + static_cast<long>(i + 2 + vlen));
    i += 2 + vlen;
  }
  core::ThreadDataset bare;
  REQUIRE(bare.set(without.data(), without.size()));
  CHECK(std::string(bare.ext_pan_id()).empty());
}
