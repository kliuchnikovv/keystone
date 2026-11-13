// The brain's own Matter commissioner, and the bridge from "a Matter node joined our
// fabric" to ordinary hub entities.
//
// Why this file exists: brain-s3 carries the CHIP stack (it has BLE and PSRAM), so the
// hub commissions Matter devices itself instead of asking the Thread co-processor to do
// it. A commissioned node is not a "chip" with a UART: it is a pseudo-chip (0x03) whose
// messages are produced and consumed in this process. Everything the hub's core sees is
// the same DECLARE/STATE/COMMAND/CMD_ACK it sees from the real co-processors, which is
// what keeps the core radio-free and untangled from CHIP.
//
// Threads: CHIP calls back on its own task (commissioning result, attribute reports). A
// single worker task owns every CHIP call (pair, read Descriptor, subscribe, invoke) and
// every publish into the hub bus. Callbacks only enqueue; nothing touches the entity
// table from two tasks at once.
//
// This header deliberately has no CHIP includes: main.cpp can use it without dragging
// the Matter SDK into its own translation unit, and a PlatformIO build (no Matter) simply
// compiles the .cpp to nothing.
#pragma once

#include <cstddef>
#include <cstdint>

#include "hubproto/message.hpp"
#include "hubproto/payload.hpp"

namespace matter_bridge {

// The pseudo-chip id of "the Matter nodes the brain itself commissioned". 0x01 and 0x02
// are the two UART co-processors; a commissioned node lands here, so every entity keeps
// one owner and the core's routing needs no special case.
inline constexpr std::uint8_t kChipId = 0x03;

// First entity id of the Matter pseudo-chip (0x0300..), mirroring the Zigbee chip's
// 0x0100 range. Four entities per node, four nodes: enough for a home's Matter devices
// and small enough to keep the persisted table tiny.
inline constexpr std::uint16_t kFirstEntityId = 0x0300;

// Deliver a message into the hub bus as if chip 0x03 had sent it (DECLARE, STATE,
// CMD_ACK). main.cpp points this at ChipRouter's inbound injection.
using Sink = void (*)(void* ctx, const hubproto::Header& h, const std::uint8_t* payload,
                      std::size_t len);

// Borrow the Thread network's operational dataset from the co-processor that has the
// radio (chip 0x02). Called on the worker task, which may block for `timeout_ms`.
using DatasetFn = bool (*)(void* ctx, std::uint8_t* out, std::size_t cap, std::size_t& len,
                           std::uint32_t timeout_ms);

// How the attempt reported by `matter.commission` is going. main.cpp turns these into the
// app's `matter` push and `matter.status`.
using ProgressFn = void (*)(void* ctx, hubproto::MatterState state, hubproto::MatterCode code,
                            std::uint16_t entity);

// Create the worker queue/task. Called early, while the internal heap is one block.
bool start(void* ctx, Sink sink, DatasetFn dataset, ProgressFn progress);

// The CHIP controller/commissioner exists (set after setup_commissioner()).
void controller_ready();
bool ready();

// Re-read the descriptors of the nodes saved in NVS (called once, after the hub's own
// tables exist: the DECLARE/STATE messages need the registry and the app queue). A node
// whose descriptor cannot be read stays saved and is tried again on the next boot.
void restore();

// Re-send every DECLARE (and DECLARE_END) for the entities the bridge knows. The core asks
// for this with a QUERY when it (re)discovers the pseudo-chip, exactly as it does for a
// co-processor that rebooted.
void republish();

// How `commission_payload` went. Kept apart from a bool because the app must not be told
// "another attempt is running" when the real answer is "this payload carries no passcode":
// a QR code is the only payload that can start PASE.
enum class Start : std::uint8_t {
  kStarted,
  kNotReady,     // the commissioner is not up (yet)
  kBusy,         // an attempt is already queued or running
  kNoSetupHash,  // kind 0/1: carries the device's identity but not its setup passcode
  kBadPayload,   // kind 2, but the bytes are not a setup payload we can parse
};

// `matter.commission` from the app: the QR payload bytes (kind 2).
Start commission_payload(std::uint8_t kind, const std::uint8_t* bytes, std::size_t len);

// The console's explicit forms: a passcode + discriminator (the label's numbers), or a
// manual/QR payload string. `with_dataset` hands the co-processor's Thread network over.
bool commission_pin(std::uint64_t node_id, std::uint32_t pincode, std::uint16_t discriminator,
                    bool with_dataset);
bool commission_code(const char* code, bool with_dataset, std::uint64_t node_id);

// Abort an attempt that is still running (the BLE scan waits ~60 s).
void cancel();

// A COMMAND from the hub core for an entity of chip 0x03. `payload` is the COMMAND body:
// entity_id(u16), attr_id(u8), value. Answers CMD_ACK for `seq` once CHIP accepted the
// invoke/write. Returns false when the entity/attribute is not a Matter binding (the
// dispatcher then reports Unreachable without pretending).
bool command(std::uint16_t seq, const std::uint8_t* payload, std::size_t len);

}  // namespace matter_bridge
