// The brain's own Matter commissioner. See matter_bridge.hpp for why this exists and for
// the shape of the interface; this file is the half that knows CHIP.
//
// One task owns every CHIP call (commission, read Descriptor, subscribe, invoke/write) and
// every publish into the hub bus. CHIP's own callbacks only enqueue small requests, so the
// entity table is touched by exactly one task. The one exception is the descriptor read,
// whose reports land in a static buffer (only one read is in flight at a time) and whose
// completion is handed to the worker as a request.

#include "matter_bridge.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <optional>
#include <utility>

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <nvs.h>

#include <esp_matter.h>
#include <esp_matter_client.h>
#include <esp_matter_controller_client.h>
#include <esp_matter_controller_cluster_command.h>
#include <esp_matter_controller_pairing_command.h>
#include <esp_matter_controller_read_command.h>
#include <esp_matter_controller_subscribe_command.h>
#include <esp_matter_controller_utils.h>
#include <esp_matter_controller_write_command.h>

#include <app-common/zap-generated/cluster-objects.h>
#include <app/data-model/Decode.h>
#include <lib/core/TLVReader.h>

#include "core/app_protocol.hpp"
#include "core/matter_entity.hpp"
#include "core/matter_onboarding.hpp"
#include "hubproto/version.hpp"

namespace matter_bridge {
namespace {

constexpr const char* kTag = "matter_bridge";

// The pseudo-chip's boot id. It never changes inside one brain boot, so the core never
// invalidates the declarations behind our back.
constexpr std::uint8_t kBootId = 1;

// Four nodes, four entities each (matter_entity.hpp's kNodeEntities); entity ids live in
// the 0x0300 range, mirroring the Zigbee chip's 0x0100 one.
constexpr std::size_t kSlots = 4;
constexpr std::size_t kEntitiesPerNode = core::matter::kNodeEntities;
constexpr std::size_t kAttrsPerEntity = core::matter::kNodeEntityAttrs;

// A node id we mint for a device added from the app. Console commissioning supplies its
// own; both end up in the same table.
constexpr std::uint64_t kNodeIdBase = 0x00414B5300000000ULL;  // "AKS" in the high bytes

// The Descriptor cluster and the two attributes that tell us what an endpoint is.
constexpr std::uint16_t kDescriptorEndpointWildcard = 0xFFFF;
constexpr std::uint32_t kDescriptorCluster = 0x001D;
constexpr std::uint32_t kDeviceTypeListAttribute = 0x0000;
constexpr std::uint32_t kServerListAttribute = 0x0001;

// What a descriptor read collects: a handful of endpoints per node, and only the device
// types and cluster ids this bridge can use. Bigger than any device a home has, small
// enough to stay in internal DRAM.
constexpr std::size_t kMaxEndpoints = 12;
constexpr std::size_t kMaxDeviceTypes = 4;
constexpr std::size_t kMaxClusters = 16;

// How long the worker waits for the co-processor's Thread dataset (the console's own
// command used 15 s) and for a descriptor read to finish (CHIP's own interaction timeout
// is longer, but a device that says nothing must not pin the worker forever).
constexpr std::uint32_t kDatasetWaitMs = 15000;

// A heartbeat every five seconds keeps ChipMonitor from declaring the pseudo-chip lost
// (silent_after_ms is 20 s). State reports from a sleepy sensor are far rarer than that.
constexpr std::uint32_t kHeartbeatMs = 5000;

// One attribute of one entity and where it lives on the Matter node.
struct RuntimeAttr {
  std::uint8_t attr = 0;
  hubproto::ValueType type = hubproto::ValueType::Bool;
  std::uint16_t endpoint = 0;
  std::uint32_t cluster = 0;
  std::uint32_t path = 0;
};

struct RuntimeEntity {
  std::uint16_t id = 0;
  hubproto::DeviceType device_type = hubproto::DeviceType::Sensor;
  std::uint8_t attr_count = 0;
  RuntimeAttr attrs[kAttrsPerEntity] = {};
};

struct RuntimeNode {
  bool used = false;
  std::uint64_t node_id = 0;
  std::uint8_t entity_count = 0;
  RuntimeEntity entities[kEntitiesPerNode] = {};
};

RuntimeNode g_nodes[kSlots];

// ---- the queue between CHIP callbacks and the worker -------------------------------------
enum class Op : std::uint8_t {
  kCommissionQr = 1,     // the app's QR bytes
  kCommissionPin,        // console: node id + passcode + discriminator
  kCommissionCode,       // console: a manual code / MT: string
  kCancel,               // abort the attempt in flight
  kCommand,              // a COMMAND from the hub core
  kAttribute,            // a value reported by a subscription
  kDescriptorDone,       // the descriptor read finished; the data is in g_build
  kDescriptorFailed,     // it did not
  kCommissioned,         // CHIP finished the commissioning of a node
  kCommissionFailed,     // ... and it failed
  kRestoreNext,          // read the descriptor of the next saved node
  kRepublish,            // re-send every DECLARE (the core asked with QUERY)
};

struct Request {
  Op op = Op::kCancel;
  std::uint16_t seq = 0;
  std::uint64_t node_id = 0;
  std::uint32_t pincode = 0;
  std::uint16_t discriminator = 0;
  std::uint8_t kind = 0;   // payload kind, or the type of an attribute report
  std::uint8_t len = 0;
  std::uint8_t bytes[hubproto::kMatterMaxPayload] = {};
  // An attribute report, in the shape the TLV layer handed it over.
  std::uint16_t endpoint = 0;
  std::uint32_t cluster = 0;
  std::uint32_t path = 0;
  std::int64_t signed_value = 0;
  std::uint64_t unsigned_value = 0;
  bool boolean = false;
  // The CHIP error of a failed attempt, as a small code (see attr_code).
  std::uint8_t code = 0;
};

QueueHandle_t g_queue = nullptr;
TaskHandle_t g_task = nullptr;
bool g_started = false;

void* g_ctx = nullptr;
Sink g_sink = nullptr;
DatasetFn g_dataset = nullptr;
ProgressFn g_progress = nullptr;

std::atomic<bool> g_controller_ready{false};
std::atomic<bool> g_attempt_active{false};

// Worker-only: the slot/node reserved for the attempt in flight and whether it has been
// saved (a saved slot survives a failed descriptor read so the next boot retries it).
int g_attempt_slot = -1;
bool g_attempt_saved = false;
std::uint64_t g_pair_node = 0;

// The NVS table is read on the worker too, not in restore(): the worker may already be
// republishing (a QUERY from the core answers the first heartbeat), and g_nodes belongs to it.
bool g_slots_loaded = false;

// ---- descriptor collection (CHIP task writes, worker reads after kDescriptorDone) --------
struct DescriptorEndpoint {
  std::uint16_t endpoint = 0;
  std::uint8_t device_type_count = 0;
  std::uint8_t cluster_count = 0;
  std::uint16_t device_types[kMaxDeviceTypes] = {};
  std::uint32_t clusters[kMaxClusters] = {};
};

struct DescriptorBuild {
  bool active = false;
  std::uint64_t node_id = 0;
  std::uint8_t slot = 0;
  std::uint8_t endpoint_count = 0;
  DescriptorEndpoint endpoints[kMaxEndpoints] = {};
};
DescriptorBuild g_build;

// ---- publishing into the hub bus ---------------------------------------------------------
void report(hubproto::MatterState state, hubproto::MatterCode code, std::uint16_t entity) {
  ESP_LOGI(kTag, "matter: %s (%s), entity 0x%04x", core::app::matter_state_name(state),
           core::app::matter_code_name(code), static_cast<unsigned>(entity));
  if (g_progress != nullptr) g_progress(g_ctx, state, code, entity);
}

void publish(const hubproto::Header& h, const std::uint8_t* payload, std::size_t len) {
  if (g_sink != nullptr) g_sink(g_ctx, h, payload, len);
}

hubproto::Header header(hubproto::MsgType type, std::uint16_t entity, std::uint16_t seq = 0) {
  hubproto::Header h{};
  h.version = hubproto::kProtocolVersion;
  h.type = type;
  h.chip_id = kChipId;
  h.boot_id = kBootId;
  h.seq = seq;
  h.entity_id = entity;
  return h;
}

void publish_declare(const RuntimeEntity& entity) {
  hubproto::AttrDecl attrs[kAttrsPerEntity] = {};
  for (std::uint8_t i = 0; i < entity.attr_count; ++i) {
    attrs[i] = hubproto::AttrDecl{entity.attrs[i].attr, entity.attrs[i].type, 0};
    attrs[i].flags = hubproto::kFlagReadable | hubproto::kFlagReportable;
    const core::matter::Binding* b = core::matter::binding_for_attr(entity.attrs[i].attr);
    if (b != nullptr) attrs[i].flags = b->flags;
  }
  std::uint8_t buf[3 + kAttrsPerEntity * 3] = {};
  const auto enc = hubproto::encode_declare(entity.id, entity.device_type, attrs,
                                            entity.attr_count, buf, sizeof(buf));
  if (!enc.ok) return;
  publish(header(hubproto::MsgType::Declare, entity.id), buf, enc.len);
}

void publish_declare_end() {
  publish(header(hubproto::MsgType::DeclareEnd, 0), nullptr, 0);
}

void publish_state(std::uint16_t entity, std::uint8_t attr, const hubproto::Value& value) {
  const hubproto::StateEntry entry{attr, value};
  std::uint8_t buf[2 + 1 + 1 + 4] = {};
  const auto enc = hubproto::encode_state(entity, &entry, 1, buf, sizeof(buf));
  if (!enc.ok) return;
  publish(header(hubproto::MsgType::State, entity), buf, enc.len);
}

void publish_ack(std::uint16_t seq, std::uint16_t entity, hubproto::CmdAckStatus status) {
  std::uint8_t buf[1] = {};
  const auto enc = hubproto::encode_cmd_ack(status, buf, sizeof(buf));
  if (!enc.ok) return;
  publish(header(hubproto::MsgType::CmdAck, entity, seq), buf, enc.len);
}

void publish_heartbeat() {
  publish(header(hubproto::MsgType::Heartbeat, 0), nullptr, 0);
}

// ---- NVS ---------------------------------------------------------------------------------
constexpr const char* kNodeNamespace = "matter";
constexpr const char* kNodesKey = "nodes";
constexpr std::uint32_t kNodesMagic = 0x4D54524EU;  // "MTRN"

struct SavedNodes {
  std::uint32_t magic;
  std::uint64_t node_id[kSlots];
};

void save_slots() {
  SavedNodes saved{};
  saved.magic = kNodesMagic;
  for (std::size_t i = 0; i < kSlots; ++i) {
    if (g_nodes[i].used) saved.node_id[i] = g_nodes[i].node_id;
  }
  nvs_handle_t handle;
  if (nvs_open(kNodeNamespace, NVS_READWRITE, &handle) != ESP_OK) {
    ESP_LOGW(kTag, "cannot save the Matter node table");
    return;
  }
  const esp_err_t err = nvs_set_blob(handle, kNodesKey, &saved, sizeof(saved));
  if (err == ESP_OK) nvs_commit(handle);
  nvs_close(handle);
  if (err != ESP_OK) ESP_LOGW(kTag, "cannot save the Matter node table: %s", esp_err_to_name(err));
}

void load_slots() {
  SavedNodes saved{};
  std::size_t len = sizeof(saved);
  nvs_handle_t handle;
  if (nvs_open(kNodeNamespace, NVS_READONLY, &handle) != ESP_OK) return;
  const esp_err_t err = nvs_get_blob(handle, kNodesKey, &saved, &len);
  nvs_close(handle);
  if (err != ESP_OK || len != sizeof(saved) || saved.magic != kNodesMagic) return;
  for (std::size_t i = 0; i < kSlots; ++i) {
    if (saved.node_id[i] == 0) continue;
    g_nodes[i].used = true;
    g_nodes[i].node_id = saved.node_id[i];
    ESP_LOGI(kTag, "restoring Matter node 0x%016llx in slot %u",
             static_cast<unsigned long long>(saved.node_id[i]), static_cast<unsigned>(i));
  }
}

// ---- slots -------------------------------------------------------------------------------
int find_slot(std::uint64_t node_id) {
  for (std::size_t i = 0; i < kSlots; ++i) {
    if (g_nodes[i].used && g_nodes[i].node_id == node_id) return static_cast<int>(i);
  }
  return -1;
}

int find_free_slot() {
  for (std::size_t i = 0; i < kSlots; ++i) {
    if (!g_nodes[i].used) return static_cast<int>(i);
  }
  return -1;
}

int reserve_slot(std::uint64_t node_id) {
  int slot = find_slot(node_id);
  if (slot >= 0) return slot;
  slot = find_free_slot();
  if (slot < 0) return -1;
  g_nodes[slot].used = true;
  g_nodes[slot].node_id = node_id;
  g_nodes[slot].entity_count = 0;
  return slot;
}

RuntimeEntity* find_entity(std::uint16_t entity_id, RuntimeNode** node_out) {
  for (auto& node : g_nodes) {
    if (!node.used) continue;
    for (std::uint8_t i = 0; i < node.entity_count; ++i) {
      if (node.entities[i].id == entity_id) {
        if (node_out != nullptr) *node_out = &node;
        return &node.entities[i];
      }
    }
  }
  return nullptr;
}

RuntimeAttr* find_attr(RuntimeEntity& entity, std::uint8_t attr) {
  for (std::uint8_t i = 0; i < entity.attr_count; ++i) {
    if (entity.attrs[i].attr == attr) return &entity.attrs[i];
  }
  return nullptr;
}

RuntimeAttr* find_reported_attr(RuntimeNode& node, std::uint16_t endpoint, std::uint32_t cluster,
                                std::uint32_t path, RuntimeEntity** entity_out) {
  for (std::uint8_t e = 0; e < node.entity_count; ++e) {
    RuntimeEntity& entity = node.entities[e];
    for (std::uint8_t a = 0; a < entity.attr_count; ++a) {
      const RuntimeAttr& attr = entity.attrs[a];
      if (attr.endpoint == endpoint && attr.cluster == cluster && attr.path == path) {
        if (entity_out != nullptr) *entity_out = &entity;
        return &entity.attrs[a];
      }
    }
  }
  return nullptr;
}

// ---- attempts ----------------------------------------------------------------------------
void release_attempt(bool free_slot) {
  if (free_slot && g_attempt_slot >= 0) {
    RuntimeNode& node = g_nodes[g_attempt_slot];
    if (!g_attempt_saved && node.entity_count == 0) {
      node.used = false;
      node.node_id = 0;
    }
  }
  g_attempt_slot = -1;
  g_attempt_saved = false;
  g_pair_node = 0;
  g_attempt_active.store(false);
}

void fail_attempt(hubproto::MatterCode code) {
  release_attempt(true);
  report(hubproto::MatterState::Failed, code, 0);
}

// The device joined our fabric, but nothing on it is something the hub models (a Matter-only
// appliance, an over-the-air updater, …). Release the slot for the next device instead of
// keeping a reservation that would never declare anything; the node itself stays on the CHIP
// fabric (the hub cannot unpair it yet — see docs/matter-backlog.md).
void forget_slot(std::uint8_t slot) {
  if (slot >= kSlots) return;
  g_nodes[slot] = RuntimeNode{};
  save_slots();
}

// ---- descriptor read ---------------------------------------------------------------------
void start_descriptor_read(std::uint8_t slot) {
  RuntimeNode& node = g_nodes[slot];
  g_build = DescriptorBuild{};
  g_build.active = true;
  g_build.node_id = node.node_id;
  g_build.slot = slot;

  using chip::app::AttributePathParams;
  using chip::Platform::ScopedMemoryBufferWithSize;

  ScopedMemoryBufferWithSize<AttributePathParams> paths;
  ScopedMemoryBufferWithSize<EventPathParams> events;
  paths.Alloc(2);
  events.Alloc(0);
  if (paths.Get() == nullptr) {
    g_build.active = false;
    ESP_LOGE(kTag, "no memory for the Descriptor read");
    if (g_attempt_active.load()) fail_attempt(hubproto::MatterCode::Failed);
    return;
  }
  paths[0] = AttributePathParams(kDescriptorEndpointWildcard, kDescriptorCluster, kDeviceTypeListAttribute);
  paths[1] = AttributePathParams(kDescriptorEndpointWildcard, kDescriptorCluster, kServerListAttribute);

  auto on_attribute = [](std::uint64_t remote_node,
                         const chip::app::ConcreteDataAttributePath& path,
                         chip::TLV::TLVReader* data, const chip::app::StatusIB& status) {
    if (!g_build.active || remote_node != g_build.node_id || data == nullptr) return;
    if (status.ToChipError() != CHIP_NO_ERROR) return;
    if (path.mClusterId != kDescriptorCluster) return;
    if (path.mAttributeId != kDeviceTypeListAttribute && path.mAttributeId != kServerListAttribute) return;

    DescriptorEndpoint* endpoint = nullptr;
    for (std::uint8_t i = 0; i < g_build.endpoint_count; ++i) {
      if (g_build.endpoints[i].endpoint == path.mEndpointId) {
        endpoint = &g_build.endpoints[i];
        break;
      }
    }
    if (endpoint == nullptr) {
      if (g_build.endpoint_count >= kMaxEndpoints) return;
      endpoint = &g_build.endpoints[g_build.endpoint_count++];
      endpoint->endpoint = path.mEndpointId;
    }

    if (path.mAttributeId == kDeviceTypeListAttribute) {
      using List =
          chip::app::Clusters::Descriptor::Attributes::DeviceTypeList::TypeInfo::DecodableType;
      List list;
      if (chip::app::DataModel::Decode(*data, list) != CHIP_NO_ERROR) return;
      auto it = list.begin();
      while (it.Next() && endpoint->device_type_count < kMaxDeviceTypes) {
        endpoint->device_types[endpoint->device_type_count++] =
            static_cast<std::uint16_t>(it.GetValue().deviceType);
      }
    } else {
      using List = chip::app::Clusters::Descriptor::Attributes::ServerList::TypeInfo::DecodableType;
      List list;
      if (chip::app::DataModel::Decode(*data, list) != CHIP_NO_ERROR) return;
      auto it = list.begin();
      while (it.Next() && endpoint->cluster_count < kMaxClusters) {
        endpoint->clusters[endpoint->cluster_count++] = static_cast<std::uint32_t>(it.GetValue());
      }
    }
  };

  auto on_done = [](std::uint64_t remote_node, const ScopedMemoryBufferWithSize<AttributePathParams>&,
                    const ScopedMemoryBufferWithSize<EventPathParams>&) {
    if (!g_build.active || remote_node != g_build.node_id) return;
    g_build.active = false;
    Request req;
    req.op = Op::kDescriptorDone;
    req.node_id = remote_node;
    if (xQueueSend(g_queue, &req, pdMS_TO_TICKS(50)) != pdTRUE) {
      ESP_LOGE(kTag, "the request queue is full: the Descriptor result is lost");
    }
  };

  auto on_error = [](std::uint64_t remote_node, CHIP_ERROR error) {
    ESP_LOGW(kTag, "Descriptor read failed: %s", chip::ErrorStr(error));
    if (!g_build.active || remote_node != g_build.node_id) return;
    g_build.active = false;
    Request req;
    req.op = Op::kDescriptorFailed;
    req.node_id = remote_node;
    if (xQueueSend(g_queue, &req, pdMS_TO_TICKS(50)) != pdTRUE) {
      ESP_LOGE(kTag, "the request queue is full: the Descriptor failure is lost");
    }
  };

  auto on_connect_failure = [](void*, const chip::ScopedNodeId& peer, CHIP_ERROR error) {
    ESP_LOGW(kTag, "cannot reach Matter node 0x%016llx for its Descriptor: %s",
             static_cast<unsigned long long>(peer.GetNodeId()), chip::ErrorStr(error));
    if (!g_build.active || peer.GetNodeId() != g_build.node_id) return;
    g_build.active = false;
    Request req;
    req.op = Op::kDescriptorFailed;
    req.node_id = peer.GetNodeId();
    if (xQueueSend(g_queue, &req, pdMS_TO_TICKS(50)) != pdTRUE) {
      ESP_LOGE(kTag, "the request queue is full: the Descriptor failure is lost");
    }
  };

  auto* cmd = chip::Platform::New<esp_matter::controller::read_command>(
      node.node_id, std::move(paths), std::move(events), on_attribute, on_done, nullptr,
      on_connect_failure, on_error);
  if (cmd == nullptr) {
    g_build.active = false;
    ESP_LOGE(kTag, "no memory for the Descriptor read command");
    if (g_attempt_active.load()) fail_attempt(hubproto::MatterCode::Failed);
    return;
  }

  const esp_err_t err = [&] {
    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
    return cmd->send_command();
  }();
  if (err != ESP_OK) {
    g_build.active = false;
    ESP_LOGE(kTag, "cannot start the Descriptor read: %s", esp_err_to_name(err));
    if (g_attempt_active.load()) fail_attempt(hubproto::MatterCode::Failed);
  }
}

// ---- subscription ------------------------------------------------------------------------
void on_attribute_report(std::uint64_t node_id, const chip::app::ConcreteDataAttributePath& path,
                         chip::TLV::TLVReader* data, const chip::app::StatusIB& status) {
  if (data == nullptr || status.ToChipError() != CHIP_NO_ERROR) return;

  Request req;
  req.op = Op::kAttribute;
  req.node_id = node_id;
  req.endpoint = path.mEndpointId;
  req.cluster = path.mClusterId;
  req.path = path.mAttributeId;

  const chip::TLV::TLVType type = data->GetType();
  if (type == chip::TLV::kTLVType_Boolean) {
    req.kind = 1;
    if (data->Get(req.boolean) != CHIP_NO_ERROR) return;
  } else if (type == chip::TLV::kTLVType_SignedInteger) {
    req.kind = 2;
    if (data->Get(req.signed_value) != CHIP_NO_ERROR) return;
  } else if (type == chip::TLV::kTLVType_UnsignedInteger) {
    req.kind = 3;
    if (data->Get(req.unsigned_value) != CHIP_NO_ERROR) return;
  } else {
    return;
  }

  // A full queue means a slow worker, not a lost subscription: the next report (or the
  // next read) brings the same value again.
  if (xQueueSend(g_queue, &req, 0) != pdTRUE) {
    ESP_LOGW(kTag, "attribute report dropped: the request queue is full");
  }
}

void subscribe_node(RuntimeNode& node) {
  using chip::app::AttributePathParams;
  using chip::app::EventPathParams;
  using chip::Platform::ScopedMemoryBufferWithSize;

  std::size_t count = 0;
  for (std::uint8_t e = 0; e < node.entity_count; ++e) count += node.entities[e].attr_count;
  if (count == 0) return;

  ScopedMemoryBufferWithSize<AttributePathParams> paths;
  ScopedMemoryBufferWithSize<EventPathParams> events;
  paths.Alloc(count);
  if (paths.Get() == nullptr) {
    ESP_LOGE(kTag, "no memory for the subscription paths");
    return;
  }
  std::size_t n = 0;
  for (std::uint8_t e = 0; e < node.entity_count; ++e) {
    for (std::uint8_t a = 0; a < node.entities[e].attr_count; ++a) {
      const RuntimeAttr& attr = node.entities[e].attrs[a];
      paths[n++] = AttributePathParams(attr.endpoint, attr.cluster, attr.path);
    }
  }

  auto on_terminated = [](std::uint64_t remote_node, std::uint32_t subscription_id) {
    ESP_LOGW(kTag, "subscription 0x%08x to node 0x%016llx ended", subscription_id,
             static_cast<unsigned long long>(remote_node));
  };
  auto on_connect_failure = [](void*, const chip::ScopedNodeId& peer, CHIP_ERROR error) {
    ESP_LOGW(kTag, "cannot subscribe to Matter node 0x%016llx: %s",
             static_cast<unsigned long long>(peer.GetNodeId()), chip::ErrorStr(error));
  };

  auto* cmd = chip::Platform::New<esp_matter::controller::subscribe_command>(
      node.node_id, std::move(paths), std::move(events), /*min_interval=*/1,
      /*max_interval=*/60, /*auto_resubscribe=*/true, on_attribute_report, nullptr, nullptr,
      +on_terminated, +on_connect_failure, /*keep_subscription=*/true);
  if (cmd == nullptr) {
    ESP_LOGE(kTag, "no memory for the subscription command");
    return;
  }
  const esp_err_t err = [&] {
    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
    return cmd->send_command();
  }();
  if (err != ESP_OK) ESP_LOGE(kTag, "cannot subscribe: %s", esp_err_to_name(err));
}

// ---- what a completed descriptor means ---------------------------------------------------
void handle_descriptor(bool ok) {
  const bool attempt = g_attempt_active.load();
  if (!ok || g_build.endpoint_count == 0) {
    ESP_LOGW(kTag, "node 0x%016llx did not describe itself",
             static_cast<unsigned long long>(g_build.node_id));
    if (attempt) {
      // The device joined our fabric but we cannot model it. The slot is saved (that
      // happened when the commissioning succeeded), so the next boot tries again instead
      // of leaving an invisible node behind.
      release_attempt(false);
      report(hubproto::MatterState::Failed, hubproto::MatterCode::Failed, 0);
    }
    if (!attempt) {
      Request req;
      req.op = Op::kRestoreNext;
      xQueueSend(g_queue, &req, 0);
    }
    return;
  }

  core::matter::EndpointView views[kMaxEndpoints];
  for (std::uint8_t i = 0; i < g_build.endpoint_count; ++i) {
    views[i].endpoint_id = g_build.endpoints[i].endpoint;
    views[i].device_types = g_build.endpoints[i].device_types;
    views[i].device_type_count = g_build.endpoints[i].device_type_count;
    views[i].clusters = g_build.endpoints[i].clusters;
    views[i].cluster_count = g_build.endpoints[i].cluster_count;
  }
  const core::matter::NodePlan plan =
      core::matter::plan_node(views, g_build.endpoint_count);
  if (!plan.known) {
    ESP_LOGW(kTag, "node 0x%016llx has nothing the hub models",
             static_cast<unsigned long long>(g_build.node_id));
    forget_slot(g_build.slot);
    if (attempt) {
      release_attempt(false);
      report(hubproto::MatterState::Failed, hubproto::MatterCode::Unsupported, 0);
    } else {
      Request req{Op::kRestoreNext};
      xQueueSend(g_queue, &req, 0);
    }
    return;
  }

  RuntimeNode& node = g_nodes[g_build.slot];
  node.entity_count = plan.entity_count;
  for (std::uint8_t e = 0; e < plan.entity_count; ++e) {
    RuntimeEntity& entity = node.entities[e];
    entity.id = static_cast<std::uint16_t>(kFirstEntityId + g_build.slot * kEntitiesPerNode + e);
    entity.device_type = plan.entities[e].device_type;
    entity.attr_count = plan.entities[e].attr_count;
    for (std::uint8_t a = 0; a < entity.attr_count; ++a) {
      const core::matter::NodeAttr& src = plan.entities[e].attrs[a];
      entity.attrs[a] = RuntimeAttr{src.attr, src.type, src.endpoint, src.cluster, src.path};
    }
  }

  for (std::uint8_t e = 0; e < node.entity_count; ++e) publish_declare(node.entities[e]);
  publish_declare_end();
  subscribe_node(node);

  ESP_LOGI(kTag, "node 0x%016llx -> %u entities, first 0x%04x",
           static_cast<unsigned long long>(node.node_id), static_cast<unsigned>(node.entity_count),
           static_cast<unsigned>(node.entities[0].id));

  if (attempt) {
    g_attempt_active.store(false);
    g_attempt_slot = -1;
    g_attempt_saved = false;
    g_pair_node = 0;
    report(hubproto::MatterState::Done, hubproto::MatterCode::Ok, node.entities[0].id);
  }
  Request req{Op::kRestoreNext};
  xQueueSend(g_queue, &req, 0);
}

void start_next_restore() {
  if (g_attempt_active.load()) return;
  if (g_build.active) return;
  for (std::size_t i = 0; i < kSlots; ++i) {
    if (g_nodes[i].used && g_nodes[i].node_id != 0 && g_nodes[i].entity_count == 0) {
      start_descriptor_read(static_cast<std::uint8_t>(i));
      return;
    }
  }
}

// ---- commands from the hub core ----------------------------------------------------------
void handle_command(const Request& req) {
  RuntimeNode* node = nullptr;
  RuntimeEntity* entity = nullptr;
  const std::uint16_t entity_id =
      static_cast<std::uint16_t>(req.bytes[0] | (static_cast<std::uint16_t>(req.bytes[1]) << 8));
  const std::uint8_t attr_id = req.bytes[2];

  // Present only when the entity is ours; find_entity() returns it without the owner.
  for (auto& n : g_nodes) {
    if (!n.used) continue;
    for (std::uint8_t i = 0; i < n.entity_count; ++i) {
      if (n.entities[i].id == entity_id) {
        node = &n;
        entity = &n.entities[i];
        break;
      }
    }
    if (entity != nullptr) break;
  }
  if (entity == nullptr) {
    publish_ack(req.seq, entity_id, hubproto::CmdAckStatus::Unsupported);
    return;
  }
  RuntimeAttr* attr = find_attr(*entity, attr_id);
  if (attr == nullptr) {
    publish_ack(req.seq, entity_id, hubproto::CmdAckStatus::Unsupported);
    return;
  }

  hubproto::Value value{};
  if (!hubproto::read_value(req.bytes + 3, static_cast<std::size_t>(req.len) - 3,
                            attr->type, value)) {
    publish_ack(req.seq, entity_id, hubproto::CmdAckStatus::Fail);
    return;
  }
  const core::matter::WritePlan plan = core::matter::to_matter(attr_id, value, attr->endpoint);
  if (!plan.ok()) {
    publish_ack(req.seq, entity_id, hubproto::CmdAckStatus::Unsupported);
    return;
  }

  char json[96] = {};
  std::size_t used = 0;
  used += static_cast<std::size_t>(std::snprintf(json + used, sizeof(json) - used, "{"));
  for (std::uint8_t i = 0; i < plan.field_count; ++i) {
    const core::matter::MatterField& f = plan.fields[i];
    used += static_cast<std::size_t>(
        std::snprintf(json + used, sizeof(json) - used, "%s\"%u:", i == 0 ? "" : ",", f.tag));
    switch (f.value.type) {
      case hubproto::ValueType::Bool:
        used += static_cast<std::size_t>(std::snprintf(
            json + used, sizeof(json) - used, "BOOL\":%s", f.value.as_bool() ? "true" : "false"));
        break;
      case hubproto::ValueType::U8:
        used += static_cast<std::size_t>(std::snprintf(
            json + used, sizeof(json) - used, "U8\":%u", static_cast<unsigned>(f.value.as_u8())));
        break;
      case hubproto::ValueType::U16:
        used += static_cast<std::size_t>(std::snprintf(
            json + used, sizeof(json) - used, "U16\":%u", static_cast<unsigned>(f.value.as_u16())));
        break;
      case hubproto::ValueType::I16:
        used += static_cast<std::size_t>(std::snprintf(
            json + used, sizeof(json) - used, "I16\":%d", static_cast<int>(f.value.as_i16())));
        break;
      default:
        publish_ack(req.seq, entity_id, hubproto::CmdAckStatus::Unsupported);
        return;
    }
  }
  std::snprintf(json + used, sizeof(json) - used, "}");

  esp_err_t err = ESP_FAIL;
  {
    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
    if (plan.kind == core::matter::SendKind::kInvoke) {
      err = esp_matter::controller::send_invoke_cluster_command(
          node->node_id, plan.endpoint, plan.cluster, plan.id, json);
    } else {
      err = esp_matter::controller::send_write_attr_command(
          node->node_id, plan.endpoint, plan.cluster, plan.id, json);
    }
  }
  if (err != ESP_OK) {
    ESP_LOGW(kTag, "entity 0x%04x attr 0x%02x did not reach node 0x%016llx: %s",
             static_cast<unsigned>(entity_id), static_cast<unsigned>(attr_id),
             static_cast<unsigned long long>(node->node_id), esp_err_to_name(err));
  }
  publish_ack(req.seq, entity_id,
              err == ESP_OK ? hubproto::CmdAckStatus::Ok : hubproto::CmdAckStatus::Fail);
}

// ---- attribute reports -------------------------------------------------------------------
void handle_attribute(const Request& req) {
  for (auto& node : g_nodes) {
    if (!node.used || node.node_id != req.node_id) continue;
    RuntimeEntity* entity = nullptr;
    RuntimeAttr* attr =
        find_reported_attr(node, req.endpoint, req.cluster, req.path, &entity);
    if (attr == nullptr || entity == nullptr) return;

    core::matter::MatterValue mv;
    if (req.kind == 1) {
      mv.boolean = req.boolean;
    } else if (req.kind == 2) {
      mv.signed_int = req.signed_value;
    } else {
      mv.unsigned_int = req.unsigned_value;
    }
    hubproto::Value value{};
    if (!core::matter::to_hub(attr->attr, mv, value)) return;
    publish_state(entity->id, attr->attr, value);
    return;
  }
}

// ---- commissioning -----------------------------------------------------------------------
void abort_pairing_locked() {
  auto& client = esp_matter::controller::matter_controller_client::get_instance();
  auto* commissioner = client.get_commissioner();
  if (commissioner == nullptr) return;
  if (commissioner->GetPairingDelegate() == nullptr) return;
  if (g_pair_node != 0) {
    const CHIP_ERROR err = commissioner->StopPairing(g_pair_node);
    ESP_LOGI(kTag, "stopped the previous pairing: %s", chip::ErrorStr(err));
  }
  commissioner->RegisterPairingDelegate(nullptr);
}

void on_pase_complete(CHIP_ERROR error) {
  if (error == CHIP_NO_ERROR) {
    report(hubproto::MatterState::Commissioning, hubproto::MatterCode::Ok, 0);
  } else {
    ESP_LOGW(kTag, "PASE failed: %s", chip::ErrorStr(error));
  }
}

void on_commissioned(chip::ScopedNodeId peer) {
  Request req;
  req.op = Op::kCommissioned;
  req.node_id = peer.GetNodeId();
  if (xQueueSend(g_queue, &req, pdMS_TO_TICKS(50)) != pdTRUE) {
    ESP_LOGE(kTag, "the request queue is full: cannot finish the commissioning");
  }
}

void on_commission_failed(chip::ScopedNodeId peer, CHIP_ERROR error,
                          chip::Controller::CommissioningStage stage,
                          std::optional<chip::Credentials::AttestationVerificationResult> extra) {
  (void)peer;
  (void)stage;
  (void)extra;
  ESP_LOGW(kTag, "commissioning failed: %s", chip::ErrorStr(error));
  Request req;
  req.op = Op::kCommissionFailed;
  req.code = (error == CHIP_ERROR_TIMEOUT) ? static_cast<std::uint8_t>(hubproto::MatterCode::Timeout)
                                           : static_cast<std::uint8_t>(hubproto::MatterCode::Failed);
  if (xQueueSend(g_queue, &req, pdMS_TO_TICKS(50)) != pdTRUE) {
    ESP_LOGE(kTag, "the request queue is full: cannot report the commissioning failure");
  }
}

void set_callbacks() {
  esp_matter::controller::pairing_command_callbacks_t callbacks{};
  callbacks.pase_callback = &on_pase_complete;
  callbacks.commissioning_success_callback = &on_commissioned;
  callbacks.commissioning_failure_callback = &on_commission_failed;
  callbacks.unpair_complete_callback = nullptr;
  esp_matter::controller::pairing_command::get_instance().set_callbacks(callbacks);
}

void start_attempt(Request& req) {
  ESP_LOGI(kTag, "commissioning: %s", req.op == Op::kCommissionQr ? "QR payload"
                                    : req.op == Op::kCommissionPin ? "passcode + discriminator"
                                                                   : "pairing code");

  std::uint64_t node_id = req.node_id;
  if (req.op == Op::kCommissionQr) {
    const int slot = find_free_slot();
    if (slot < 0) {
      release_attempt(false);
      report(hubproto::MatterState::Failed, hubproto::MatterCode::Busy, 0);
      return;
    }
    node_id = kNodeIdBase + static_cast<std::uint64_t>(slot) + 1;
  } else if (node_id == 0) {
    release_attempt(false);
    report(hubproto::MatterState::Failed, hubproto::MatterCode::BadPayload, 0);
    return;
  }

  const int slot = reserve_slot(node_id);
  if (slot < 0) {
    release_attempt(false);
    report(hubproto::MatterState::Failed, hubproto::MatterCode::Busy, 0);
    return;
  }
  g_attempt_slot = slot;
  g_attempt_saved = false;
  g_pair_node = node_id;

  core::matter::Onboarding onboarding;
  if (req.op == Op::kCommissionQr) {
    onboarding = core::matter::parse_onboarding_bytes(req.bytes, req.len);
    if (!onboarding.valid) {
      fail_attempt(hubproto::MatterCode::BadPayload);
      return;
    }
  }

  report(hubproto::MatterState::Scanning, hubproto::MatterCode::Ok, 0);

  // A Thread device needs the network of the co-processor that has the radio. Fetch it
  // before taking the CHIP lock: the answer arrives on the UART task and needs the bus.
  //
  // The buffer is static, not on the stack: CHIP keeps the pointer for the whole
  // commissioning (minutes), and this task is the only reader/writer of it — the next
  // attempt cannot start before this one has reported its end.
  static std::uint8_t dataset[hubproto::kThreadDatasetMax];
  std::size_t dataset_len = 0;
  bool want_dataset = true;
  if (req.op == Op::kCommissionPin) want_dataset = (req.len != 0);   // console's "with dataset"
  if (req.op == Op::kCommissionCode) want_dataset = (req.kind != 0);
  bool have_dataset = false;
  if (g_dataset != nullptr && want_dataset) {
    // For the QR path the dataset is best effort: a Wi-Fi Matter device does not need it.
    have_dataset = g_dataset(g_ctx, dataset, sizeof(dataset), dataset_len, kDatasetWaitMs);
    if (have_dataset) {
      ESP_LOGI(kTag, "using the co-processor's Thread dataset (%u bytes)",
               static_cast<unsigned>(dataset_len));
    } else if (req.op != Op::kCommissionQr) {
      fail_attempt(hubproto::MatterCode::Failed);
      return;
    }
  }

  esp_err_t err = ESP_FAIL;
  {
    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
    abort_pairing_locked();
    set_callbacks();
    switch (req.op) {
      case Op::kCommissionQr:
        err = esp_matter::controller::pairing_ble_thread(
            node_id, onboarding.passcode, onboarding.discriminator, dataset,
            static_cast<std::uint8_t>(have_dataset ? dataset_len : 0));
        break;
      case Op::kCommissionPin:
        err = esp_matter::controller::pairing_ble_thread(
            node_id, req.pincode, req.discriminator, dataset,
            static_cast<std::uint8_t>(have_dataset ? dataset_len : 0));
        break;
      case Op::kCommissionCode:
        err = have_dataset
                  ? esp_matter::controller::pairing_code_thread(
                        node_id, reinterpret_cast<const char*>(req.bytes), dataset,
                        static_cast<std::uint8_t>(dataset_len))
                  : esp_matter::controller::pairing_code(
                        node_id, reinterpret_cast<const char*>(req.bytes));
        break;
      default:
        break;
    }
  }
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "cannot start the commissioning: %s", esp_err_to_name(err));
    {
      esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
      abort_pairing_locked();
    }
    fail_attempt(hubproto::MatterCode::Failed);
  }
}

void handle_commissioned(std::uint64_t node_id) {
  int slot = find_slot(node_id);
  if (slot < 0) slot = reserve_slot(node_id);
  if (slot < 0) {
    ESP_LOGE(kTag, "no free slot for node 0x%016llx", static_cast<unsigned long long>(node_id));
    release_attempt(false);
    report(hubproto::MatterState::Failed, hubproto::MatterCode::Busy, 0);
    return;
  }
  g_attempt_slot = slot;
  g_attempt_saved = true;
  save_slots();
  report(hubproto::MatterState::Adding, hubproto::MatterCode::Ok, 0);
  start_descriptor_read(static_cast<std::uint8_t>(slot));
}

// ---- the worker --------------------------------------------------------------------------
void worker(void*) {
  for (;;) {
    Request req;
    if (xQueueReceive(g_queue, &req, pdMS_TO_TICKS(kHeartbeatMs)) != pdTRUE) {
      if (g_controller_ready.load()) publish_heartbeat();
      continue;
    }

    switch (req.op) {
      case Op::kCommissionQr:
      case Op::kCommissionPin:
      case Op::kCommissionCode:
        start_attempt(req);
        break;

      case Op::kCancel: {
        esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
        abort_pairing_locked();
        if (g_attempt_active.load()) {
          release_attempt(true);
          report(hubproto::MatterState::Failed, hubproto::MatterCode::Failed, 0);
        }
        break;
      }

      case Op::kCommand:
        handle_command(req);
        break;

      case Op::kAttribute:
        handle_attribute(req);
        break;

      case Op::kDescriptorDone:
        handle_descriptor(true);
        break;

      case Op::kDescriptorFailed:
        handle_descriptor(false);
        break;

      case Op::kCommissioned:
        handle_commissioned(req.node_id);
        break;

      case Op::kCommissionFailed: {
        const auto code = static_cast<hubproto::MatterCode>(req.code);
        if (g_attempt_active.load()) fail_attempt(code);
        break;
      }

      case Op::kRestoreNext:
        if (!g_slots_loaded) {
          g_slots_loaded = true;
          load_slots();
        }
        start_next_restore();
        break;

      case Op::kRepublish: {
        for (auto& node : g_nodes) {
          if (!node.used) continue;
          for (std::uint8_t e = 0; e < node.entity_count; ++e) publish_declare(node.entities[e]);
        }
        publish_declare_end();
        break;
      }
    }
  }
}

}  // namespace

bool start(void* ctx, Sink sink, DatasetFn dataset, ProgressFn progress) {
  if (g_started) return true;
  g_ctx = ctx;
  g_sink = sink;
  g_dataset = dataset;
  g_progress = progress;

  g_queue = xQueueCreate(16, sizeof(Request));
  if (g_queue == nullptr) {
    ESP_LOGE(kTag, "no memory for the request queue");
    return false;
  }
  // The commissioning path is a deep CHIP call chain (PAKE, BLE, crypto) and overflowed
  // the console task's 8 KB; this task has the room and owns every CHIP call.
  if (xTaskCreate(worker, "matter_bridge", 12288 / sizeof(StackType_t), nullptr, 5, &g_task) !=
      pdPASS) {
    ESP_LOGE(kTag, "could not create the worker task");
    vQueueDelete(g_queue);
    g_queue = nullptr;
    return false;
  }
  g_started = true;
  ESP_LOGI(kTag, "bridge up (worker task, %u slots)", static_cast<unsigned>(kSlots));
  return true;
}

void controller_ready() {
  g_controller_ready.store(true);
  ESP_LOGI(kTag, "commissioner ready");
}

bool ready() { return g_started && g_controller_ready.load(); }

void restore() {
  if (!g_started) return;
  // The NVS table and the descriptors are the worker's business; this only asks it to start.
  Request req{Op::kRestoreNext};
  xQueueSend(g_queue, &req, 0);
}

void republish() {
  if (!g_started) return;
  Request req{Op::kRepublish};
  xQueueSend(g_queue, &req, 0);
}

Start commission_payload(std::uint8_t kind, const std::uint8_t* bytes, std::size_t len) {
  if (!ready()) return Start::kNotReady;
  if (kind != hubproto::kMatterKindQr) {
    // Service data and the enhanced advertisement identify a device but carry no setup
    // passcode: PASE cannot start without the QR code's one. The caller turns this into
    // the app's error — no attempt was started, so nothing is pushed to the app.
    return Start::kNoSetupHash;
  }
  const core::matter::Onboarding onboarding = core::matter::parse_onboarding(kind, bytes, len);
  if (!onboarding.valid) return Start::kBadPayload;

  bool expected = false;
  if (!g_attempt_active.compare_exchange_strong(expected, true)) return Start::kBusy;

  Request req;
  req.op = Op::kCommissionQr;
  req.node_id = 0;
  req.len = static_cast<std::uint8_t>(len > sizeof(req.bytes) ? sizeof(req.bytes) : len);
  std::memcpy(req.bytes, bytes, req.len);
  if (xQueueSend(g_queue, &req, 0) != pdTRUE) {
    g_attempt_active.store(false);
    return Start::kBusy;
  }
  return Start::kStarted;
}

bool commission_pin(std::uint64_t node_id, std::uint32_t pincode, std::uint16_t discriminator,
                    bool with_dataset) {
  if (!ready() || node_id < 2 || pincode == 0 ||
      pincode > core::matter::kMaxSetupPasscode || discriminator > 0xFFF) {
    return false;
  }
  bool expected = false;
  if (!g_attempt_active.compare_exchange_strong(expected, true)) return false;

  Request req;
  req.op = Op::kCommissionPin;
  req.node_id = node_id;
  req.pincode = pincode;
  req.discriminator = discriminator;
  // The worker uses len != 0 as "this attempt needs a dataset" for the pin path.
  req.len = with_dataset ? 1 : 0;
  if (xQueueSend(g_queue, &req, 0) != pdTRUE) {
    g_attempt_active.store(false);
    return false;
  }
  return true;
}

bool commission_code(const char* code, bool with_dataset, std::uint64_t node_id) {
  if (!ready() || code == nullptr || node_id < 2) return false;
  const std::size_t len = std::strlen(code);
  if (len == 0 || len >= hubproto::kMatterMaxPayload) return false;
  bool expected = false;
  if (!g_attempt_active.compare_exchange_strong(expected, true)) return false;

  Request req;
  req.op = Op::kCommissionCode;
  req.node_id = node_id;
  req.len = static_cast<std::uint8_t>(len);
  std::memcpy(req.bytes, code, len);
  req.bytes[len] = '\0';
  // The worker uses a separate bit for "hand the Thread dataset over": the payload is
  // already in bytes[], so the flag cannot ride in len here.
  req.kind = with_dataset ? 1 : 0;
  if (xQueueSend(g_queue, &req, 0) != pdTRUE) {
    g_attempt_active.store(false);
    return false;
  }
  return true;
}

void cancel() {
  if (!g_started) return;
  Request req{Op::kCancel};
  xQueueSend(g_queue, &req, 0);
}

bool command(std::uint16_t seq, const std::uint8_t* payload, std::size_t len) {
  if (!g_started || payload == nullptr || len < 3 || len > sizeof(Request::bytes)) return false;
  Request req;
  req.op = Op::kCommand;
  req.seq = seq;
  req.len = static_cast<std::uint8_t>(len);
  std::memcpy(req.bytes, payload, len);
  return xQueueSend(g_queue, &req, 0) == pdTRUE;
}

}  // namespace matter_bridge
