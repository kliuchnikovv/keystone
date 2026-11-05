#include <doctest/doctest.h>

#include <cstring>
#include <string>
#include <vector>

#include "core/thread_dataset.hpp"

namespace {

// A minimal but complete-looking dataset: channel 15, PAN id, extended PAN id,
// network key, security policy and a network name.
std::vector<std::uint8_t> good_dataset() {
  std::vector<std::uint8_t> d;
  auto tlv = [&d](std::uint8_t type, const std::vector<std::uint8_t>& value) {
    d.push_back(type);
    d.push_back(static_cast<std::uint8_t>(value.size()));
    d.insert(d.end(), value.begin(), value.end());
  };
  tlv(core::kTlvActiveTimestamp, {0, 0, 0, 0, 0, 0, 0, 1});
  tlv(core::kTlvChannel, {15, 0, 0});
  tlv(core::kTlvPanId, {0x12, 0x34});
  tlv(core::kTlvExtPanId, {1, 2, 3, 4, 5, 6, 7, 8});
  tlv(core::kTlvMeshLocalPrefix, {0xfd, 0, 0, 0, 0, 0, 0, 0});
  tlv(core::kTlvNetworkKey, {0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9});
  tlv(core::kTlvSecurityPolicy, {0, 0, 0});
  tlv(core::kTlvNetworkName, {'O', 'p', 'e', 'n', 'T', 'h', 'r', 'e', 'a', 'd'});
  return d;
}

}  // namespace

TEST_CASE("the dataset: a well-formed one is accepted, with the channel and the name read out") {
  const auto bytes = good_dataset();
  core::ThreadDataset ds;
  REQUIRE(ds.set(bytes.data(), bytes.size()));
  CHECK(ds.valid());
  CHECK(ds.size() == bytes.size());
  CHECK(ds.channel() == 15);
  CHECK(std::string(ds.network_name()) == "OpenThread");
  CHECK(ds.has(core::kTlvNetworkKey));
  CHECK(ds.has(core::kTlvExtPanId));
  CHECK_FALSE(ds.has(0x42));
  CHECK(std::memcmp(ds.data(), bytes.data(), bytes.size()) == 0);
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
  const std::uint8_t over[] = {0x03, 0x02, 0x12};
  CHECK_FALSE(ds.set(over, sizeof(over)));

  // A header cut in half: a type with no length.
  const std::uint8_t half_header[] = {0x02, 0x03, 15, 0x00, 0x00, 0x03, 0x02, 0x12, 0x34, 0x0E};
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
  CHECK_FALSE(ds.set(without(core::kTlvNetworkKey).data(), without(core::kTlvNetworkKey).size()));
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
}
