// Thread network co-processor (ESP32-C6, chip_id 0x02).
//
// The home's Thread network lives here. The brain runs the Matter commissioner
// over BLE, but it has no 802.15.4 radio: it can neither form a Thread network
// nor give one to a device it commissions. This co-processor has the radio, so
// it forms (and keeps) the network and lends its operational dataset to the
// brain over the hub link — hubproto ThreadDatasetReq (0x62) / ThreadDataset
// (0x63). The brain hands those very bytes to the device in the commissioning
// step (docs/matter-backlog.md §3).
//
// What it is not (any more): a Matter Bridge. The Apple Home bridge is gone.
// The CHIP stack it needs does not fit this chip (the bridge alone took
// 190 KB of static DRAM, docs/matter-backlog.md §3), and the bridge stood in
// the way of the goal: it needs the CHIP stack, the CHIP stack keeps the
// Thread role disabled until the chip sits in a Matter fabric, and the Thread
// role is exactly what this chip is for.
//
// UART bridge: chip 0x02, HELLO on boot, heartbeat every 5 s, our own
// telemetry every 5 s, the chip-self DECLARE when the brain asks (QUERY).
//
// Wiring: C6.TX(GPIO5) -> S3.RX(GPIO9), C6.RX(GPIO4) <- S3.TX(GPIO8).
#include <cstdint>
#include <cstring>

#include "esp_console.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "esp_openthread.h"
#include "esp_openthread_border_router.h"
#include "esp_openthread_lock.h"
#include "esp_openthread_netif_glue.h"
#include "esp_openthread_types.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "esp_vfs_eventfd.h"
#include "esp_coexist.h"
#include "soc/soc_caps.h"
#include "mdns.h"
#include "wifi_secrets.h"
#include <openthread/border_routing.h>
#include <openthread/dataset.h>
#include <openthread/dataset_ftd.h>
#include <openthread/error.h>
#include <openthread/instance.h>
#include <openthread/ip6.h>
#include <openthread/thread.h>
#include <openthread/thread_ftd.h>

#include "hal/brownout_ll.h"
#include "esp_ota_sink.hpp"
#include "esp_uart_link.hpp"
#include "nvs_boot_id.hpp"
#include "core/ota_receiver.hpp"
#include "core/uart_transport.hpp"
#include "hubproto/message.hpp"
#include "hubproto/payload.hpp"
#include "hubproto/version.hpp"
#include "status_led.hpp"

namespace {

const char* kTag = "thread-c6";

constexpr int kUartTxGpio = 5;
constexpr int kUartRxGpio = 4;
constexpr std::uint8_t kOwnChipId = 0x02;

// UartTransport::publish uses one shared encode buffer, but we publish from the
// main loop and from the UART RX task (OTA acks) — serialize it.
class LockedBus final : public core::Bus {
 public:
  explicit LockedBus(core::Bus& inner) : inner_(inner), mu_(xSemaphoreCreateRecursiveMutex()) {}
  bool publish(const hubproto::Header& h, const std::uint8_t* p,
               std::size_t n) noexcept override {
    xSemaphoreTakeRecursive(mu_, portMAX_DELAY);
    const bool ok = inner_.publish(h, p, n);
    xSemaphoreGiveRecursive(mu_);
    return ok;
  }
  bool subscribe(MessageFn fn, void* ctx) noexcept override { return inner_.subscribe(fn, ctx); }

 private:
  core::Bus& inner_;
  SemaphoreHandle_t mu_;
};

core::Bus* g_bus = nullptr;
fw::OtaConfirmGate g_ota_confirm;
std::uint8_t g_boot_id = 0;
std::uint16_t g_next_seq = 1;

// Wi-Fi backbone + Border Router state. Declared here because thread_bring_up
// (further down) needs them; the Wi-Fi bringup code itself is in a section
// below it.
esp_netif_t* g_wifi_netif = nullptr;
SemaphoreHandle_t g_wifi_got_ip = nullptr;
bool g_wifi_connected = false;
bool g_br_started = false;

hubproto::Header header(hubproto::MsgType type,
                        std::uint16_t entity = hubproto::kSelfEntityId,
                        std::uint16_t seq = 0) {
  hubproto::Header h{};
  h.version = hubproto::kProtocolVersion;
  h.type = type;
  h.chip_id = kOwnChipId;
  h.boot_id = g_boot_id;
  h.seq = seq ? seq : g_next_seq++;
  h.entity_id = entity;
  return h;
}

// --- The Thread network we lend to the brain --------------------------------
//
// The network must survive a reboot of this chip: the devices we commissioned
// are attached to its PAN id and network key, and a fresh key would leave them
// behind (they would have to be re-commissioned). So the operational dataset
// lives in NVS; the Thread port keeps its own copy of the dataset as well, but
// we keep our own because that is the one we hand to the brain and because it
// survives a reflash that wipes the port's settings.

constexpr char kThreadNs[] = "thread_net";
constexpr char kThreadDsKey[] = "ds";
constexpr char kThreadNetName[] = "keystone-home";

// The Thread channel. The Wi-Fi backbone of this chip sits on channel 6 of the
// home network (2417-2457 MHz), and the C6 has one radio for Wi-Fi and
// 802.15.4 alike — so Thread has to stay out of that band. Channel 26
// (2480 MHz) is the cleanest choice.
constexpr std::uint8_t kThreadChannel = 26;

// The name this chip answers to on the backbone (mDNS). The Border Router
// publishes the Thread network under it, and the Thread services reflected
// from the mesh hang on it.
constexpr char kBrHostname[] = "keystone-br";

bool thread_load_dataset(otOperationalDatasetTlvs& out) {
  nvs_handle_t h;
  if (nvs_open(kThreadNs, NVS_READONLY, &h) != ESP_OK) return false;
  std::size_t len = sizeof(out.mTlvs);
  const esp_err_t err = nvs_get_blob(h, kThreadDsKey, out.mTlvs, &len);
  nvs_close(h);
  if (err != ESP_OK || len == 0 || len > OT_OPERATIONAL_DATASET_MAX_LENGTH) return false;
  out.mLength = static_cast<std::uint8_t>(len);
  return true;
}

void thread_save_dataset(const otOperationalDatasetTlvs& tlvs) {
  nvs_handle_t h;
  if (nvs_open(kThreadNs, NVS_READWRITE, &h) != ESP_OK) return;
  esp_err_t err = nvs_set_blob(h, kThreadDsKey, tlvs.mTlvs, tlvs.mLength);
  if (err == ESP_OK) err = nvs_commit(h);
  nvs_close(h);
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "could not save the Thread dataset: %d", static_cast<int>(err));
  }
}

void thread_forget_dataset() {
  nvs_handle_t h;
  if (nvs_open(kThreadNs, NVS_READWRITE, &h) != ESP_OK) return;
  nvs_erase_key(h, kThreadDsKey);
  nvs_commit(h);
  nvs_close(h);
}

const char* role_name(otDeviceRole role) {
  switch (role) {
    case OT_DEVICE_ROLE_DISABLED: return "disabled";
    case OT_DEVICE_ROLE_DETACHED: return "detached";
    case OT_DEVICE_ROLE_CHILD: return "child";
    case OT_DEVICE_ROLE_ROUTER: return "router";
    case OT_DEVICE_ROLE_LEADER: return "leader";
    default: return "unknown";
  }
}

// How many devices are attached to us right now. A commissioned Matter device
// shows up here as a child — that is the moment we know it joined our network.
int thread_child_count() {
  otInstance* ot = esp_openthread_get_instance();
  if (ot == nullptr) return 0;
  int count = 0;
  for (std::uint16_t i = 0; i < 32; ++i) {
    otChildInfo info{};
    if (otThreadGetChildInfoByIndex(ot, i, &info) != OT_ERROR_NONE) break;
    ++count;
  }
  return count;
}

// The children of our network, one line each.
//
// This is the diagnostic for the last step of the commissioning: the device
// joins the Thread network as our child, and from then on the brain talks to
// it through us. Two numbers matter here:
//
//   * the age (seconds since we last heard the child) — a child that stopped
//     answering is a device that left or went to sleep for good;
//   * the queued messages — packets that arrived for the child but could not
//     be handed over yet (a sleepy end device only picks them up when it
//     polls). A number that grows while the brain is sending means the traffic
//     reaches us and the device is the one that is not taking it.
//
// The IPv6 addresses are printed too: the mesh-local address of the child is
// the address the brain sends the CASE packets to, and if the two disagree
// the packets go nowhere.
void thread_print_children(otInstance* ot) {
  for (std::uint16_t i = 0; i < 32; ++i) {
    otChildInfo info{};
    if (otThreadGetChildInfoByIndex(ot, i, &info) != OT_ERROR_NONE) break;
    printf("  child %u: ieee %02x%02x%02x%02x%02x%02x%02x%02x rloc=%04x rssi=%d age=%us queued=%u%s\n",
           static_cast<unsigned>(i),
           info.mExtAddress.m8[0], info.mExtAddress.m8[1], info.mExtAddress.m8[2],
           info.mExtAddress.m8[3], info.mExtAddress.m8[4], info.mExtAddress.m8[5],
           info.mExtAddress.m8[6], info.mExtAddress.m8[7],
           static_cast<unsigned>(info.mRloc16), static_cast<int>(info.mLastRssi),
           static_cast<unsigned>(info.mAge),
           static_cast<unsigned>(info.mQueuedMessageCnt),
           info.mRxOnWhenIdle ? "" : " sleepy");
    otChildIp6AddressIterator iter = OT_CHILD_IP6_ADDRESS_ITERATOR_INIT;
    for (;;) {
      otIp6Address addr{};
      if (otThreadGetChildNextIp6Address(ot, i, &iter, &addr) != OT_ERROR_NONE) break;
      char text[48] = {0};
      otIp6AddressToString(&addr, text, sizeof(text));
      printf("    ipv6 %s\n", text);
    }
  }
}

// What the Border Router tells the LAN about, and what it sees coming in.
//
// The commissioning talks to the device over the mesh, so the traffic has to
// take this way: the brain sends a packet to the device's mesh-local address,
// the LAN routes it to us (the BR publishes the OMR prefix upstream, the LAN
// learns it from our router advertisement), and we forward it into the mesh.
//
// Three numbers say where a packet got lost:
//   * the mesh-local prefix — the prefix the devices carry their addresses in;
//   * the OMR prefix — the route we publish upward, so the LAN hands us the
//     traffic for the mesh; it has to be the same prefix as the one the
//     devices use, or the LAN routes to us a prefix nobody is in;
//   * the counters — unicast packets that came in from the backbone (the CASE
//     packets of the brain) and the ones that went back out.
// The two fields of the dataset that say which network this is: the extended
// PAN id (the identity of the network itself) and the mesh-local prefix (the
// prefix the devices carry their addresses in — it is a random ULA of its own,
// independent of the extended PAN id).
void thread_print_dataset_ids(const otOperationalDatasetTlvs& tlvs) {
  constexpr std::uint8_t kTlvExtPanId = 0x02;
  constexpr std::uint8_t kTlvMeshLocalPrefix = 0x07;
  std::uint8_t i = 0;
  while (i + 2 <= tlvs.mLength) {
    const std::uint8_t type = tlvs.mTlvs[i];
    const std::uint8_t len = tlvs.mTlvs[i + 1];
    const std::uint8_t* value = &tlvs.mTlvs[i + 2];
    if (i + 2 + len > tlvs.mLength) break;
    if (type == kTlvExtPanId && len == 8) {
      printf("ext pan id:         %02x%02x%02x%02x%02x%02x%02x%02x\n",
             value[0], value[1], value[2], value[3], value[4], value[5], value[6], value[7]);
    } else if (type == kTlvMeshLocalPrefix && len == 8) {
      printf("dataset ml prefix:  %02x%02x%02x%02x:%02x%02x:%02x%02x::/64\n",
             value[0], value[1], value[2], value[3], value[4], value[5], value[6], value[7]);
    }
    i = static_cast<std::uint8_t>(i + 2 + len);
  }
}

// Every TLV of the dataset, one line each. We hand these bytes to the device
// during the commissioning (the brain passes them to CHIP verbatim), so what is
// (and is not) in here decides what the device joins: the mesh-local prefix it
// gets, and whether the partition id survives a reboot of this chip.
void thread_print_dataset_tlvs(const otOperationalDatasetTlvs& tlvs) {
  // The MeshCoP TLV types of an operational dataset (openthread/dataset.h).
  const char* kName[] = {"channel",           "pan id",        "ext pan id",
                         "network name",      "pskc",          "master key",
                         "key sequence",      "mesh local prefix", "steering data",
                         "border agent rloc", "commissioner id", "commission session id",
                         "security policy",   "active timestamp"};
  std::uint8_t i = 0;
  while (i + 2 <= tlvs.mLength) {
    const std::uint8_t type = tlvs.mTlvs[i];
    const std::uint8_t len = tlvs.mTlvs[i + 1];
    const std::uint8_t* value = &tlvs.mTlvs[i + 2];
    if (i + 2 + len > tlvs.mLength) {
      printf("  tlv 0x%02x: truncated\n", type);
      break;
    }
    printf("  tlv 0x%02x (%-18s) %u bytes:", type,
           type < sizeof(kName) / sizeof(kName[0]) ? kName[type] : "?", len);
    for (std::uint8_t k = 0; k < len; ++k) printf(" %02x", value[k]);
    printf("\n");
    i = static_cast<std::uint8_t>(i + 2 + len);
  }
}

void thread_print_br(otInstance* ot) {
  char text[48] = {0};
  const otMeshLocalPrefix* mlp = otThreadGetMeshLocalPrefix(ot);
  if (mlp != nullptr) {
    // The mesh-local prefix comes back as the eight bytes of a network prefix;
    // the printer wants a full prefix with a length.
    otIp6Prefix ml{};
    ml.mPrefix.mFields.mComponents.mNetworkPrefix = *mlp;
    ml.mLength = 64;
    otIp6PrefixToString(&ml, text, sizeof(text));
    printf("mesh-local prefix: %s\n", text);
  } else {
    printf("mesh-local prefix: none\n");
  }
  otIp6Prefix omr{};
  const bool have_omr = otBorderRoutingGetOmrPrefix(ot, &omr) == OT_ERROR_NONE;
  if (have_omr) {
    otIp6PrefixToString(&omr, text, sizeof(text));
    printf("omr prefix:        %s\n", text);
    // The OMR prefix is only advertised upstream (as a route) when the leader's
    // network data carries it. Without it the LAN has no way into the mesh.
    printf("omr in netdata:    %s\n",
           otNetDataContainsOmrPrefix(ot, &omr) ? "yes" : "NO (the LAN cannot route into the mesh)");
  } else {
    printf("omr prefix:        none\n");
  }
  // The on-link prefix is what the Border Router offers the hosts on the
  // backbone (the brain): they take an address out of it, and the BR maps it
  // into the mesh. It is the address the brain's packets to the mesh come from.
  otIp6Prefix onlink{};
  if (otBorderRoutingGetOnLinkPrefix(ot, &onlink) == OT_ERROR_NONE) {
    otIp6PrefixToString(&onlink, text, sizeof(text));
    printf("on-link prefix:    %s\n", text);
  } else {
    printf("on-link prefix:    none\n");
  }
  // Every prefix the mesh publishes: the routes the leader knows about.
  otBorderRouterConfig entry{};
  otNetworkDataIterator iterator = OT_NETWORK_DATA_ITERATOR_INIT;
  int n = 0;
  while (otNetDataGetNextOnMeshPrefix(ot, &iterator, &entry) == OT_ERROR_NONE) {
    otIp6PrefixToString(&entry.mPrefix, text, sizeof(text));
    printf("  on-mesh prefix:   %s (pref=%d slaac=%d on-mesh=%d)\n", text,
           entry.mPreference, entry.mSlaac ? 1 : 0, entry.mOnMesh ? 1 : 0);
    if (++n > 16) break;
  }
  if (n == 0) printf("  on-mesh prefix:   none\n");
  // The "ot" netif is the door into the mesh: lwIP forwards a backbone packet
  // into it, and it can only resolve a next hop for a mesh address if the
  // netif carries an address of the prefix the mesh lives in. That address is
  // the one the SLAAC module forms out of the "slaac" on-mesh prefix above
  // (main/esp_ot_custom_config.h turns the module on) — without it lwIP has
  // no route into the mesh at all and drops the packet with ERR_RTE.
  printf("ot slaac:         %s\n", otIp6IsSlaacEnabled(ot) ? "on" : "off");
  esp_netif_t* ot_netif = esp_openthread_get_netif();
  if (ot_netif != nullptr) {
    esp_ip6_addr_t addrs[8]{};
    const int count = esp_netif_get_all_ip6(ot_netif, addrs);
    printf("ot netif:         %d address(es)\n", count);
    for (int i = 0; i < count && i < 8; i++) {
      otIp6Address address{};
      std::memcpy(address.mFields.m8, addrs[i].addr, sizeof(address.mFields.m8));
      char line[48] = {0};
      otIp6AddressToString(&address, line, sizeof(line));
      const bool in_omr =
          have_omr && std::memcmp(address.mFields.m8, omr.mPrefix.mFields.m8, 8) == 0;
      printf("  %s%s\n", line,
             in_omr ? "   (in the omr prefix: lwIP can route into the mesh)" : "");
    }
  } else {
    printf("ot netif:         none\n");
  }
  const otBorderRoutingCounters* c = otIp6GetBorderRoutingCounters(ot);
  if (c != nullptr) {
    printf("br counters: in_unicast=%u pkt/%u B  out_unicast=%u pkt/%u B  "
           "in_mcast=%u  out_mcast=%u\n",
           static_cast<unsigned>(c->mInboundUnicast.mPackets),
           static_cast<unsigned>(c->mInboundUnicast.mBytes),
           static_cast<unsigned>(c->mOutboundUnicast.mPackets),
           static_cast<unsigned>(c->mOutboundUnicast.mBytes),
           static_cast<unsigned>(c->mInboundMulticast.mPackets),
           static_cast<unsigned>(c->mOutboundMulticast.mPackets));
    printf("br ra: tx_ok=%u tx_fail=%u  rs: rx=%u tx_ok=%u tx_fail=%u  ra_rx=%u\n",
           static_cast<unsigned>(c->mRaTxSuccess), static_cast<unsigned>(c->mRaTxFailure),
           static_cast<unsigned>(c->mRsRx), static_cast<unsigned>(c->mRsTxSuccess),
           static_cast<unsigned>(c->mRsTxFailure), static_cast<unsigned>(c->mRaRx));
  }
}

void thread_print_status(const char* why) {
  otInstance* ot = esp_openthread_get_instance();
  if (ot == nullptr) {
    ESP_LOGI(kTag, "Thread (%s): no OpenThread instance", why);
    return;
  }
  const otDeviceRole role = otThreadGetDeviceRole(ot);
  const char* name = otThreadGetNetworkName(ot);
  ESP_LOGI(kTag, "Thread (%s): role=%s network=%s partition=0x%06x children=%d",
           why, role_name(role), name ? name : "?",
           static_cast<unsigned>(otThreadGetPartitionId(ot)), thread_child_count());
}

// The backbone services, in two parts: mDNS first (no OpenThread lock), then
// the Border Router (an OpenThread call, so it takes the lock for its own
// duration).
void thread_bring_up_mdns();
void thread_bring_up_border_router();

// Bring the network up: restore the dataset we know (or form a new one),
// enable the Thread role and lead the network so that a device we commission
// has a router in range to attach to.
void thread_bring_up() {
  otInstance* ot = esp_openthread_get_instance();
  if (ot == nullptr) {
    ESP_LOGE(kTag, "no OpenThread instance: the brain cannot borrow a Thread network");
    return;
  }

  otOperationalDatasetTlvs tlvs{};
  bool have_network = false;
  if (otDatasetGetActiveTlvs(ot, &tlvs) == OT_ERROR_NONE && tlvs.mLength > 0) {
    ESP_LOGI(kTag, "an active Thread dataset is already there (%u bytes)", tlvs.mLength);
    have_network = true;
  } else if (thread_load_dataset(tlvs)) {
    if (otDatasetSetActiveTlvs(ot, &tlvs) != OT_ERROR_NONE) {
      ESP_LOGE(kTag, "could not restore our Thread network");
      return;
    }
    ESP_LOGI(kTag, "restored our Thread network from NVS (%u bytes)", tlvs.mLength);
    have_network = true;
  }

  if (have_network) {
    // The channel is part of the dataset, and it has to be one the Wi-Fi
    // backbone can live with: the C6 has ONE radio for both, and the home
    // network sits on channel 6 (2417-2457 MHz). A Thread network on channel
    // 15 (2425 MHz) lands in the middle of that band — the two radios starve
    // each other and the Wi-Fi stops even finding the AP. Channel 26 (2480 MHz)
    // is clear of it. If the network we have is on another channel, we move it
    // (the PAN id and the keys stay, so the devices we know keep working).
    otOperationalDataset parsed{};
    if (otDatasetParseTlvs(&tlvs, &parsed) != OT_ERROR_NONE) {
      ESP_LOGW(kTag, "could not read the channel out of the dataset");
    } else if (parsed.mChannel != kThreadChannel) {
      ESP_LOGW(kTag, "the network is on channel %u, moving it to %u",
               static_cast<unsigned>(parsed.mChannel),
               static_cast<unsigned>(kThreadChannel));
      parsed.mChannel = kThreadChannel;
      parsed.mComponents.mIsChannelPresent = true;
      if (otDatasetSetActive(ot, &parsed) != OT_ERROR_NONE) {
        ESP_LOGE(kTag, "could not move the network to channel %u",
                 static_cast<unsigned>(kThreadChannel));
      } else {
        otOperationalDatasetTlvs moved{};
        if (otDatasetGetActiveTlvs(ot, &moved) == OT_ERROR_NONE) {
          thread_save_dataset(moved);
        }
      }
    }
  } else {
    // A network of our own: a random PAN id, extended PAN id, network key and
    // PSKc (otDatasetCreateNewNetwork fills those in), our name and channel.
    otOperationalDataset ds{};
    if (otDatasetCreateNewNetwork(ot, &ds) != OT_ERROR_NONE) {
      ESP_LOGE(kTag, "could not create a Thread network");
      return;
    }
    ds.mChannel = kThreadChannel;
    ds.mComponents.mIsChannelPresent = true;
    std::memset(ds.mNetworkName.m8, 0, sizeof(ds.mNetworkName.m8));
    std::memcpy(ds.mNetworkName.m8, kThreadNetName, std::strlen(kThreadNetName));
    ds.mComponents.mIsNetworkNamePresent = true;
    if (otDatasetSetActive(ot, &ds) != OT_ERROR_NONE) {
      ESP_LOGE(kTag, "could not set the new Thread dataset");
      return;
    }
    if (otDatasetGetActiveTlvs(ot, &tlvs) != OT_ERROR_NONE || tlvs.mLength == 0) {
      ESP_LOGE(kTag, "the new Thread dataset did not come back");
      return;
    }
    thread_save_dataset(tlvs);
    ESP_LOGI(kTag, "formed a Thread network '%s' (channel %u, %u bytes of dataset)",
             kThreadNetName, static_cast<unsigned>(kThreadChannel),
             static_cast<unsigned>(tlvs.mLength));
  }

  if (otIp6SetEnabled(ot, true) != OT_ERROR_NONE) {
    ESP_LOGE(kTag, "could not enable IPv6");
    return;
  }
  // The Border Router advertises its OMR prefix in the network data with the
  // "slaac" flag; the SLAAC module (compiled in by main/esp_ot_custom_config.h)
  // is what puts an address of that prefix on the "ot" netif. lwIP can then
  // resolve a next hop into the mesh — without it every backbone packet to a
  // mesh address dies with ERR_RTE before it reaches the OpenThread stack.
  // The module is on by default once it is built in; the call makes that
  // explicit and gives us the state for the log.
  otIp6SetSlaacEnabled(ot, true);
  ESP_LOGI(kTag, "OpenThread SLAAC module: %s",
           otIp6IsSlaacEnabled(ot) ? "on" : "off");
  if (otThreadSetEnabled(ot, true) != OT_ERROR_NONE) {
    ESP_LOGE(kTag, "could not enable the Thread role");
    return;
  }
  // The devices we commission need a router in range: lead our own network
  // instead of waiting for an attach attempt to time out.
  const otError err = otThreadBecomeLeader(ot);
  ESP_LOGI(kTag, "the Thread role is enabled (become leader: %d)", static_cast<int>(err));
  status_led::set(status_led::State::Ready);

  // The backbone services are not part of this function: mDNS needs the
  // OpenThread lock OFF and has to run BEFORE the Thread role comes up, see
  // thread_bring_up_mdns().
}

// The backbone services, part one: mDNS on the Wi-Fi side.
//
// It runs before the Thread role comes up, and without the OpenThread lock.
// Two reasons:
//  - mDNS is an lwIP thing: mdns_init() waits for the lwIP thread (a TCP/IP
//    API call), and the MLD report that goes out while joining ff02::fb can
//    reach the "ot" netif's transmit path, which waits for the OpenThread
//    task-switching lock. The OpenThread lock holds that very lock, so doing
//    mDNS under it means the two wait for each other and the chip freezes
//    right after "starting mDNS".
//  - The Border Router publishes its Thread network ("_meshcop") as soon as
//    the Thread role is up, and that publish needs the mDNS hostname to be
//    there already: without it the service is dropped ("Failed to publish
//    meshcop mdns service") and the LAN never hears about our network.
void thread_bring_up_mdns() {
  if (g_wifi_netif == nullptr) {
    return;
  }
  // mDNS on the Wi-Fi side: OT's DNS-SD server answers unicast DNS over
  // Thread, and espressif/mdns advertises the same records as multicast on
  // Wi-Fi — together they reflect the device's _matter._tcp onto the
  // commissioner's side.
  ESP_LOGI(kTag, "starting mDNS");
  const esp_err_t m = mdns_init();
  if (m != ESP_OK && m != ESP_ERR_INVALID_STATE) {
    ESP_LOGW(kTag, "mdns_init: %d", static_cast<int>(m));
  } else {
    ESP_LOGI(kTag, "mDNS is up");
  }
  // The hostname is NOT set by mdns_init(): the component leaves
  // s_server->hostname empty until somebody tells it who we are. Every
  // service published under it then fails SILENTLY — mdns_service_add()
  // returns ESP_ERR_INVALID_ARG without a single log line, because the
  // component has no hostname to hang the record on. The Border Router
  // publishes its Thread network ("_meshcop") exactly that way, and the
  // failure ends in "Failed to publish meshcop mdns service".
  //
  // mdns_hostname_set() is synchronous: it queues the action and waits for
  // the mDNS task to apply it, so the hostname is in place when it returns.
  ESP_LOGI(kTag, "setting the mDNS hostname");
  const esp_err_t h = mdns_hostname_set(kBrHostname);
  if (h != ESP_OK) {
    ESP_LOGE(kTag, "mdns_hostname_set(%s): %d", kBrHostname, static_cast<int>(h));
  } else {
    ESP_LOGI(kTag, "mDNS hostname is '%s'", kBrHostname);
  }
}

// The backbone services, part two: the Border Router itself — SRP server,
// DNS-SD and border routing. It is an OpenThread call, so it takes the lock
// for the duration of the call and nothing else.
void thread_bring_up_border_router() {
  if (g_wifi_netif == nullptr) {
    return;
  }
  ESP_LOGI(kTag, "starting the Border Router");
  esp_openthread_lock_acquire(portMAX_DELAY);
  const esp_err_t br = esp_openthread_border_router_init();
  esp_openthread_lock_release();
  if (br == ESP_OK) {
    ESP_LOGI(kTag, "Border Router is up (SRP server + DNS-SD + border routing)");
    g_br_started = true;
  } else {
    ESP_LOGE(kTag, "esp_openthread_border_router_init: %d", static_cast<int>(br));
  }
}

// Wait until the backbone has a usable IPv6 link-local address (defined with
// the Wi-Fi code below).
static bool wait_backbone_ipv6(std::uint32_t timeout_ms);

void thread_network_task(void* /*arg*/) {
  // The main loop has to spin before the Thread state machine may run.
  vTaskDelay(pdMS_TO_TICKS(1000));
  // Wait for Wi-Fi to come up before enabling the Thread role: on the C6,
  // the Wi-Fi and 802.15.4 radios share one antenna, and Thread is far
  // chattier than an idle Wi-Fi scan — if Thread is already an active leader
  // when the 4-way handshake runs, EAPOL packets lose airtime and auth times
  // out (reason 2). With Thread silent until we have an IP, Wi-Fi joins first
  // and coex stays sane after.
  const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(60000);
  while (!g_wifi_connected && xTaskGetTickCount() < deadline) {
    vTaskDelay(pdMS_TO_TICKS(250));
  }
  if (!g_wifi_connected) {
    ESP_LOGW(kTag, "giving up on Wi-Fi; bringing Thread up without BR");
  }
  // The Border Router needs a usable IPv6 on the backbone before it starts:
  // without it the Router Solicitation goes out without a source address
  // ("Failed to send ND6 message", "Failed to send RS 1/3") and the Thread
  // prefix is never offered to the LAN.
  wait_backbone_ipv6(10000);
#if CONFIG_ESP_COEX_SW_COEXIST_ENABLE && CONFIG_SOC_IEEE802154_SUPPORTED
  // Wi-Fi and 802.15.4 share ONE radio on the C6, and the coexistence
  // arbiter has to know that the 802.15.4 radio is there. It has to happen
  // BEFORE the Thread role is enabled: the radio goes active the moment the
  // role is on, and without the arbiter the Wi-Fi loses the access point
  // ("bcn_timeout") - the backbone dies before the Border Router is even up.
  // (The IDF's own examples/openthread/ot_br arms it right after the Border
  // Router starts; there the role is enabled by the BR itself, so the window
  // is much shorter than ours.)
  const esp_err_t coex = esp_coex_wifi_i154_enable();
  if (coex != ESP_OK) {
    ESP_LOGE(kTag, "esp_coex_wifi_i154_enable: %d", static_cast<int>(coex));
  } else {
    ESP_LOGI(kTag, "Wi-Fi / 802.15.4 coexistence is on");
  }
#endif
  // mDNS first, with the OpenThread lock OFF: it is an lwIP thing and it must
  // not run while the lock is held (deadlock, see thread_bring_up_mdns()).
  // It also has to be up before the Thread role, because the Border Router
  // publishes its "_meshcop" service the moment the role comes up.
  thread_bring_up_mdns();
  esp_openthread_lock_acquire(portMAX_DELAY);
  thread_bring_up();
  esp_openthread_lock_release();
  thread_bring_up_border_router();
  vTaskDelete(nullptr);
}

// --- Wi-Fi infrastructure (backbone for the Border Router) ------------------
//
// The Border Router needs an "infrastructure" netif — the IP network that is
// on the other side of the Thread mesh from it. Here that is Wi-Fi: the chip
// joins the same SSID as the brain, and OpenThread's BR advertises Thread
// prefixes onto Wi-Fi while reflecting SRP services (the device's
// _matter._tcp) via mDNS so the CHIP commissioner on the brain can find them.
//
// Credentials live in main/wifi_secrets.h (gitignored), same mechanism as the
// brain uses. Reconnect is handled in the event handler: the Wi-Fi driver drops
// the link whenever the AP is slow or busy, and silent reconnect keeps BR up.

int g_scan_logged = 0;

void log_wifi_scan() {
  if (g_scan_logged++) return;  // once is enough
  wifi_scan_config_t sc{};  // default: all channels, active scan
  esp_wifi_scan_start(&sc, true /*block*/);
  uint16_t n = 0;
  esp_wifi_scan_get_ap_num(&n);
  ESP_LOGW(kTag, "Wi-Fi scan found %u APs:", n);
  wifi_ap_record_t recs[16];
  uint16_t want = n < 16 ? n : 16;
  esp_wifi_scan_get_ap_records(&want, recs);
  for (uint16_t i = 0; i < want; ++i) {
    ESP_LOGW(kTag, "  ap %u: '%s' rssi=%d ch=%u auth=%d",
             i, recs[i].ssid, recs[i].rssi, recs[i].primary,
             static_cast<int>(recs[i].authmode));
  }
}

// The IPv6 link-local address of the Wi-Fi backbone.
//
// lwIP does NOT create it by itself — the application has to ask for it
// (esp_netif_create_ip6_linklocal). Without it the backbone has no IPv6 at
// all, and two things break:
//   - the Border Router cannot send a Router Solicitation/Advertisement
//     ("Failed to send ND6 message", "BorderRouting-: Failed to send RS 1/3"),
//     so the Thread prefix is never offered to the backbone;
//   - the DNS-SD server cannot publish the Thread services there
//     ("Failed to publish meshcop mdns service"), so the CHIP commissioner on
//     the brain cannot find the accessory.
//
// The address is derived from the interface's hardware address, so it can only
// be made after esp_wifi_init() has given the netif its MAC, and the interface
// has to be up (lwIP refuses otherwise).
static void wifi_ipv6_linklocal() {
  if (g_wifi_netif == nullptr) {
    return;
  }
  const esp_err_t err = esp_netif_create_ip6_linklocal(g_wifi_netif);
  if (err != ESP_OK) {
    ESP_LOGW(kTag, "no IPv6 link-local address on the backbone: %d",
             static_cast<int>(err));
  }
}

// Wait until the backbone's link-local address is usable.
//
// A fresh lwIP address is "tentative" until the duplicate address detection
// has finished (about a second). Nothing may use a tentative address, and
// esp_netif_get_ip6_linklocal() says so: it only hands out a "preferred"
// address. The Border Router has to wait for that — otherwise its Router
// Solicitation goes out with no source address ("Failed to send ND6 message").
static bool wait_backbone_ipv6(std::uint32_t timeout_ms) {
  if (g_wifi_netif == nullptr) {
    return false;
  }
  for (std::uint32_t waited = 0; waited <= timeout_ms; waited += 200) {
    esp_ip6_addr_t ll{};
    if (esp_netif_get_ip6_linklocal(g_wifi_netif, &ll) == ESP_OK) {
      ESP_LOGI(kTag, "backbone IPv6 link-local: " IPV6STR, IPV62STR(ll));
      return true;
    }
    vTaskDelay(pdMS_TO_TICKS(200));
  }
  ESP_LOGW(kTag, "the backbone has no usable IPv6 link-local address after %u ms",
           static_cast<unsigned>(timeout_ms));
  return false;
}

void wifi_event(void* /*arg*/, esp_event_base_t base, int32_t id, void* data) {
  if (base == WIFI_EVENT) {
    switch (id) {
      case WIFI_EVENT_STA_START:
        esp_wifi_connect();
        break;
      case WIFI_EVENT_STA_CONNECTED:
        // esp_netif has already brought the interface up (its own handler runs
        // first), so the link-local address can be made now.
        wifi_ipv6_linklocal();
        break;
      case WIFI_EVENT_STA_DISCONNECTED: {
        auto* e = static_cast<wifi_event_sta_disconnected_t*>(data);
        ESP_LOGW(kTag, "Wi-Fi disconnected (reason %d), retrying", e ? e->reason : -1);
        g_wifi_connected = false;
        if (e && e->reason == WIFI_REASON_NO_AP_FOUND) log_wifi_scan();
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_wifi_connect();
        break;
      }
      default:
        break;
    }
  } else if (base == IP_EVENT) {
    if (id == IP_EVENT_STA_GOT_IP) {
      auto* e = static_cast<ip_event_got_ip_t*>(data);
      ESP_LOGI(kTag, "Wi-Fi got IP " IPSTR, IP2STR(&e->ip_info.ip));
      // Idempotent: on a reconnect the address may have been dropped.
      wifi_ipv6_linklocal();
      g_wifi_connected = true;
      if (g_wifi_got_ip) xSemaphoreGive(g_wifi_got_ip);
    }
  }
}

void wifi_bringup_init() {
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  g_wifi_netif = esp_netif_create_default_wifi_sta();
  if (g_wifi_netif == nullptr) {
    ESP_LOGE(kTag, "could not create the default Wi-Fi STA netif");
    return;
  }
  wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&wcfg));

  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event, nullptr, nullptr));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event, nullptr, nullptr));

  wifi_config_t wc{};
  std::strncpy(reinterpret_cast<char*>(wc.sta.ssid), KEYSTONE_WIFI_SSID,
               sizeof(wc.sta.ssid) - 1);
  std::strncpy(reinterpret_cast<char*>(wc.sta.password), KEYSTONE_WIFI_PASSWORD,
               sizeof(wc.sta.password) - 1);
  // No authmode threshold: accept anything the AP offers (WPA2/WPA3 mixed).
  wc.sta.threshold.authmode = WIFI_AUTH_OPEN;
  // All-channel scan + pick strongest match.
  wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
  wc.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
  ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
  g_wifi_got_ip = xSemaphoreCreateBinary();
}

bool wifi_bringup_connect() {
  ESP_LOGI(kTag, "Wi-Fi joining SSID (%d chars)",
           static_cast<int>(std::strlen(KEYSTONE_WIFI_SSID)));
  // Disable Wi-Fi power save: on the C6, Wi-Fi + 802.15.4 share the one
  // antenna, and MODEM power save steals the airtime a WPA2 4-way handshake
  // needs — the AP's EAPOL messages time out and auth fails with reason 2
  // (AUTH_EXPIRE). PS_NONE keeps the receiver on and lets coex schedule
  // 802.15.4 around it.
  ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
  ESP_ERROR_CHECK(esp_wifi_start());

  // 30 s is enough for the AP; without an IP we can still run (OT keeps its
  // network), but BR stays off until an IP arrives. A silent reconnect fires
  // in the event handler.
  if (xSemaphoreTake(g_wifi_got_ip, pdMS_TO_TICKS(30000)) != pdTRUE) {
    ESP_LOGW(kTag, "no Wi-Fi yet; Border Router will start when the link is up");
    return false;
  }
  return true;
}

// Start OpenThread: the platform (radio), the instance and the main loop.
bool thread_start() {
  // The OpenThread port waits on eventfd()s (its task queue and its radio), so
  // the event VFS has to be there before the stack starts. Without it the port
  // fails to create its task queue and the radio driver is torn down again.
  //
  // How many: the netif, the task queue, the border router and — because we
  // run the radio on the chip itself — the radio driver. The IDF's own
  // examples/openthread/ot_br counts them the same way; one short and the
  // last one silently fails to open.
  esp_vfs_eventfd_config_t evcfg{};
  evcfg.max_fds = 4;
  const esp_err_t ev = esp_vfs_eventfd_register(&evcfg);
  if (ev != ESP_OK && ev != ESP_ERR_INVALID_STATE) {
    ESP_LOGE(kTag, "could not register the eventfd VFS: %d", static_cast<int>(ev));
    return false;
  }

  // esp_openthread_start() — not the older esp_openthread_init() — is what
  // creates the "ot" netif: it makes esp_netif_new() for the OpenThread
  // interface and attaches the netif glue (esp_openthread.cpp: ot_task_worker).
  // Without that netif lwIP has no interface into the mesh at all: every
  // backbone packet to a mesh address dies with ERR_RTE before it reaches the
  // OpenThread stack, and the Border Router counters stay at in_unicast=0 —
  // which is exactly how the Matter CASE session to a commissioned device
  // died ("Session establishment failed", CHIP error 32).
  //
  // The config is static on purpose: esp_openthread_start() hands the pointer
  // to its worker task, which reads it after we return.
  static esp_openthread_config_t cfg = {
      .netif_config = ESP_NETIF_DEFAULT_OPENTHREAD(),
      .platform_config = {
          .radio_config = {
              .radio_mode = RADIO_MODE_NATIVE,
          },
          .host_config = {
              .host_connection_mode = HOST_CONNECTION_MODE_NONE,
          },
          .port_config = {
              .storage_partition_name = "nvs",
              .netif_queue_size = 10,
              .task_queue_size = 10,
          },
      },
  };

  // The call blocks until the stack is up and the netif is attached; the main
  // loop then runs in the task esp_openthread_start() creates (we do not need
  // our own mainloop task any more).
  const esp_err_t err = esp_openthread_start(&cfg);
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "esp_openthread_start failed: %d", static_cast<int>(err));
    return false;
  }
  ESP_LOGI(kTag, "OpenThread is up (FTD, native radio, \"ot\" netif created)");

  // The Border Router needs the backbone netif (the Wi-Fi side) — it is the
  // infrastructure interface the BR routes through. Now that the OT stack
  // exists, hand it over; the IDF's own examples/openthread/ot_br does the
  // same right before esp_openthread_border_router_init().
  if (g_wifi_netif != nullptr) {
    esp_openthread_set_backbone_netif(g_wifi_netif);
    ESP_LOGI(kTag, "backbone netif set (Wi-Fi), BR path armed");
  } else {
    ESP_LOGW(kTag, "no backbone netif; Border Router disabled this boot");
  }

  xTaskCreate(thread_network_task, "ks_thread_net", 4096, nullptr, 5, nullptr);
  return true;
}

void on_bus_message(void* /*ctx*/, const hubproto::Header& h,
                    const std::uint8_t* payload, std::size_t len) {
  switch (h.type) {
    case hubproto::MsgType::Query: {
      g_ota_confirm.note_link_ok();  // the brain is talking to us (docs/ota.md)
      std::uint8_t sbuf[64];
      auto sr = hubproto::encode_chip_self_declare(kOwnChipId, sbuf, sizeof(sbuf));
      if (sr.ok) {
        g_bus->publish(header(hubproto::MsgType::Declare, kOwnChipId), sbuf, sr.len);
      }
      g_bus->publish(header(hubproto::MsgType::DeclareEnd), nullptr, 0);
      return;
    }
    case hubproto::MsgType::ThreadDatasetReq: {
      // The brain has no 802.15.4 radio, so the Matter commissioner on it borrows ours: it
      // asks for the active Thread dataset and hands it to the device over BLE
      // (docs/matter-backlog.md §3). Without a dataset the device refuses the network
      // (networkingStatus=1) and the commissioning dies at the Thread step.
      // The OpenThread instance belongs to the main loop task: every call into it
      // has to hold the port lock, or the main loop may be halfway through a
      // dataset update while we read it.
      esp_openthread_lock_acquire(portMAX_DELAY);
      otInstance* ot = esp_openthread_get_instance();
      if (ot == nullptr) {
        esp_openthread_lock_release();
        ESP_LOGW(kTag, "the brain asked for the Thread dataset, but there is no OpenThread");
        return;
      }
      // The dataset is the biggest thing this chip ever sends, and this runs on
      // the UART RX task: keep the two buffers out of that task's stack (it also
      // carries the OTA state machine). Only the RX task delivers inbound
      // frames, so a static buffer here has a single writer.
      static otOperationalDatasetTlvs tlvs{};
      const otError err = otDatasetGetActiveTlvs(ot, &tlvs);
      esp_openthread_lock_release();
      if (err != OT_ERROR_NONE || tlvs.mLength == 0) {
        ESP_LOGW(kTag, "no active Thread dataset to give (err %d)", static_cast<int>(err));
        return;
      }
      static std::uint8_t buf[1 + hubproto::kThreadDatasetMax];
      auto r = hubproto::encode_thread_dataset(tlvs.mTlvs, tlvs.mLength, buf, sizeof(buf));
      if (!r.ok) {
        ESP_LOGW(kTag, "the Thread dataset does not fit a frame (%u bytes)", tlvs.mLength);
        return;
      }
      // Say whether the frame really went out: this is the only big frame the chip
      // ever sends, and if it is lost the brain has nothing to show for it.
      const bool sent = g_bus->publish(header(hubproto::MsgType::ThreadDataset), buf, r.len);
      ESP_LOGI(kTag, "sent the Thread dataset (%u bytes, %u byte payload) to the brain: %s",
               tlvs.mLength, static_cast<unsigned>(r.len), sent ? "written" : "WRITE FAILED");
      return;
    }
    case hubproto::MsgType::MatterPair: {
      // We do not commission Matter devices any more — the CHIP stack is gone from this
      // chip. Say so at once instead of letting the brain wait a minute for an answer
      // that will never come.
      hubproto::MatterResult res{};
      res.state = hubproto::MatterState::Failed;
      res.code = hubproto::MatterCode::Unsupported;
      res.entity_id = 0;
      std::uint8_t buf[16];
      auto r = hubproto::encode_matter_result(res, buf, sizeof(buf));
      if (r.ok) {
        g_bus->publish(header(hubproto::MsgType::MatterResult), buf, r.len);
      }
      ESP_LOGW(kTag, "the brain asked us to commission a Matter device — we cannot");
      return;
    }
    default:
      return;
  }
}

}  // namespace

static int cmd_thread(int argc, char** argv) {
  if (argc >= 2 && std::strcmp(argv[1], "reset") == 0) {
    printf("Forgetting the Thread network; a new one is formed after the reboot.\n");
    thread_forget_dataset();
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
    return 0;
  }
  esp_openthread_lock_acquire(portMAX_DELAY);
  otInstance* ot = esp_openthread_get_instance();
  if (ot == nullptr) {
    esp_openthread_lock_release();
    printf("no OpenThread instance\n");
    return 1;
  }
  otOperationalDatasetTlvs tlvs{};
  const otError err = otDatasetGetActiveTlvs(ot, &tlvs);
  printf("role=%s network=%s partition=0x%06x dataset=%u bytes children=%d\n",
         role_name(otThreadGetDeviceRole(ot)),
         otThreadGetNetworkName(ot) ? otThreadGetNetworkName(ot) : "?",
         static_cast<unsigned>(otThreadGetPartitionId(ot)),
         err == OT_ERROR_NONE ? static_cast<unsigned>(tlvs.mLength) : 0u,
         thread_child_count());
  thread_print_children(ot);
  thread_print_dataset_ids(tlvs);
  thread_print_dataset_tlvs(tlvs);
  thread_print_br(ot);
  esp_openthread_lock_release();
  // The backbone's IPv6: without a usable link-local address the Border Router
  // cannot send a Router Solicitation and the DNS-SD server cannot publish.
  if (g_wifi_netif != nullptr) {
    esp_ip6_addr_t ll{};
    if (esp_netif_get_ip6_linklocal(g_wifi_netif, &ll) == ESP_OK) {
      printf("backbone link-local: " IPV6STR "\n", IPV62STR(ll));
    } else {
      printf("backbone link-local: none (IPv6 unusable)\n");
    }
    esp_ip6_addr_t global{};
    if (esp_netif_get_ip6_global(g_wifi_netif, &global) == ESP_OK) {
      printf("backbone global:     " IPV6STR "\n", IPV62STR(global));
    }
  }
  return 0;
}

static int cmd_factoryreset(int, char**) {
  printf("Forgetting the Thread network and rebooting...\n");
  thread_forget_dataset();
  vTaskDelay(pdMS_TO_TICKS(200));
  esp_restart();
  return 0;
}

static int cmd_reboot(int, char**) {
  printf("Rebooting...\n");
  vTaskDelay(pdMS_TO_TICKS(200));
  esp_restart();
  return 0;
}

static void start_console() {
  esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
  repl_cfg.prompt = "thread>";
  repl_cfg.max_cmdline_length = 128;
  esp_console_repl_t* repl = nullptr;
  // The C6's USB port is the USB-Serial-JTAG peripheral, and in ESP-IDF 5.5
  // that is the default console for this chip — so the REPL lives there and is
  // reachable over the very same cable we flash with (the UART0 pins of the
  // DevKitC are not wired to the USB connector). The UART branch stays for a
  // build that picks the UART console instead.
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
  esp_console_dev_usb_serial_jtag_config_t hw_cfg = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
  if (esp_console_new_repl_usb_serial_jtag(&hw_cfg, &repl_cfg, &repl) != ESP_OK) {
    ESP_LOGW(kTag, "the REPL did not come up on USB-Serial-JTAG (the log still works)");
    return;
  }
#elif CONFIG_ESP_CONSOLE_UART_DEFAULT || CONFIG_ESP_CONSOLE_UART_CUSTOM
  esp_console_dev_uart_config_t hw_cfg = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
  if (esp_console_new_repl_uart(&hw_cfg, &repl_cfg, &repl) != ESP_OK) {
    ESP_LOGW(kTag, "the REPL did not come up on UART");
    return;
  }
#else
  return;  // no console device selected — nothing to attach the REPL to
#endif
  esp_console_register_help_command();
  static const esp_console_cmd_t cmds[] = {
      {"thread",       "Show the Thread network status (reset: forget it)", nullptr, cmd_thread,
       nullptr, nullptr},
      {"factoryreset", "Forget the Thread network and reboot", nullptr, cmd_factoryreset, nullptr,
       nullptr},
      {"reboot",       "Reboot the chip", nullptr, cmd_reboot, nullptr, nullptr},
  };
  for (const auto& c : cmds) esp_console_cmd_register(&c);
  esp_console_start_repl(repl);
}

// Why did the last boot end? A watchdog, a panic or a plain brownout look the
// same from the outside (the USB port just disappears), so the reason is worth
// printing while it is still in the register.
const char* thread_reset_reason_name() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "power-on";
    case ESP_RST_EXT: return "external pin";
    case ESP_RST_SW: return "software reset";
    case ESP_RST_PANIC: return "panic/exception";
    case ESP_RST_INT_WDT: return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT: return "other watchdog";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    case ESP_RST_USB: return "USB";
    case ESP_RST_JTAG: return "JTAG";
    case ESP_RST_EFUSE: return "eFuse";
    case ESP_RST_PWR_GLITCH: return "power glitch";
    case ESP_RST_CPU_LOCKUP: return "CPU lockup";
    default: return "unknown";
  }
}

extern "C" void app_main() {
  brownout_ll_bod_enable(false);
  brownout_ll_ana_reset_enable(false);
  ESP_LOGI(kTag, "reset reason: %s", thread_reset_reason_name());
  ESP_ERROR_CHECK(nvs_flash_init());
  // Mark app valid immediately so the bootloader never rolls back to an empty
  // ota_1 slot after erase_flash. The OtaConfirmGate (30 s + brain link) still
  // protects OTA-delivered images — this only overrides the initial USB flash.
  esp_ota_mark_app_valid_cancel_rollback();
  status_led::init();
  g_boot_id = fw::next_boot_id();
  if (g_boot_id > 50) {
    ESP_LOGW(kTag, "crash loop detected (boot_id=%u > 50): erasing NVS to recover", g_boot_id);
    nvs_flash_erase();
    nvs_flash_init();
    g_boot_id = fw::next_boot_id();  // restarts at 1
  }
  start_console();

  // --- UART bridge to the brain ---
  static fw::EspUartLink link([] {
    fw::EspUartLink::Config c;
    c.port = UART_NUM_1;
    c.tx_gpio = kUartTxGpio;
    c.rx_gpio = kUartRxGpio;
    c.task_stack = 4096;  // OTA writes flash from this task (docs/ota.md)
    return c;
  }());
  ESP_ERROR_CHECK(link.begin());
  static core::UartTransport raw_bus(link);
  static LockedBus bus(raw_bus);
  g_bus = &bus;
  bus.subscribe(&on_bus_message, nullptr);

  // Firmware updates from the brain (docs/ota.md); polled from the UART RX task.
  static fw::EspOtaSink ota_sink;
  static core::OtaReceiver ota_rx(bus, ota_sink, [] {
    core::OtaReceiver::Config c;
    c.own_chip_id = kOwnChipId;
    c.boot_id = g_boot_id;
    return c;
  }());
  static auto ota_tick = [](void* ctx) {
    auto* rx = static_cast<core::OtaReceiver*>(ctx);
    const auto uptime_ms = static_cast<std::uint32_t>(esp_timer_get_time() / 1000);
    g_ota_confirm.tick(uptime_ms);
    rx->poll(uptime_ms);
    if (rx->reboot_pending()) fw::ota_reboot_into_new_image(UART_NUM_1);
  };
  link.set_tick(ota_tick, &ota_rx);
  link.start();
#ifdef KS_TEST_CRASH
  // Test image for the rollback path (docs/ota.md): crash well before the 30 s
  // confirm grace, so the bootloader must revert to the previous slot.
  xTaskCreate([](void*) { vTaskDelay(pdMS_TO_TICKS(8000)); abort(); }, "ks_crash", 2048,
              nullptr, 5, nullptr);
#endif

  ESP_LOGI(kTag, "chip 0x%02x up (Thread network); boot_id=%u", kOwnChipId,
           static_cast<unsigned>(g_boot_id));

  // Order matters on the C6: Wi-Fi + 802.15.4 share one antenna, and the
  // coexistence arbiter only schedules the radios it has been told about.
  //
  // 1) wifi_bringup_init(): esp_netif + esp_event + esp_wifi_init. Creates
  //    the netif we hand to the OT backbone. Does NOT start Wi-Fi yet.
  // 2) thread_start(): esp_openthread_set_backbone_netif(wifi_netif) +
  //    esp_openthread_start (the OT stack, its "ot" netif and the main loop).
  //    802.15.4 is now visible to coex, but the Thread role stays disabled
  //    (silent radio).
  // 3) wifi_bringup_connect(): esp_wifi_start + wait for an IP. Coex now
  //    knows about both radios and the 4-way handshake has airtime.
  // 4) thread_network_task (spawned by thread_start) waits for the IP, then
  //    enables the Thread role and calls border_router_init.
  wifi_bringup_init();
  thread_start();
  wifi_bringup_connect();

  bus.publish(header(hubproto::MsgType::Hello), nullptr, 0);

  // LED tick timer (50 ms for smooth pulsing during pairing).
  static esp_timer_handle_t led_timer;
  const esp_timer_create_args_t led_args{
      .callback = [](void*) {
        status_led::tick(static_cast<std::uint32_t>(esp_timer_get_time() / 1000));
      },
      .arg = nullptr,
      .dispatch_method = ESP_TIMER_TASK,
      .name = "led",
      .skip_unhandled_events = true,
  };
  ESP_ERROR_CHECK(esp_timer_create(&led_args, &led_timer));
  ESP_ERROR_CHECK(esp_timer_start_periodic(led_timer, 50000));

  std::uint32_t status_tick = 0;
  while (true) {
    vTaskDelay(pdMS_TO_TICKS(5000));
    const std::uint32_t now_s = static_cast<std::uint32_t>(esp_timer_get_time() / 1000000);
    hubproto::ChipSelf self{};
    self.uptime_s = now_s;
    self.heap_free = esp_get_free_heap_size();
    self.boot_id = g_boot_id;
    self.rx_frames = static_cast<std::uint32_t>(raw_bus.rx_frames());
    self.tx_frames = static_cast<std::uint32_t>(raw_bus.tx_frames());
    self.crc_errors = static_cast<std::uint16_t>(raw_bus.bad_frames());
    // Our place in the Thread network: the role and how many devices hang on
    // us. The brain shows these, so the Thread state is visible without the
    // chip's own console.
    {
      esp_openthread_lock_acquire(portMAX_DELAY);
      otInstance* ot = esp_openthread_get_instance();
      if (ot != nullptr) {
        self.thread_role = static_cast<std::uint8_t>(otThreadGetDeviceRole(ot));
        self.thread_children = static_cast<std::uint8_t>(thread_child_count());
      } else {
        self.thread_role = hubproto::chip_attr::kNoThreadRole;
      }
      esp_openthread_lock_release();
    }

    std::uint8_t buf[64];
    auto r = hubproto::encode_chip_self_state(kOwnChipId, self, buf, sizeof(buf));
    if (r.ok) {
      bus.publish(header(hubproto::MsgType::State, kOwnChipId), buf, r.len);
    }
    bus.publish(header(hubproto::MsgType::Heartbeat), nullptr, 0);

    // The Thread role changes quietly (the network comes up, a device attaches),
    // so say where we stand every half minute.
    if (++status_tick % 6 == 0) {
      esp_openthread_lock_acquire(portMAX_DELAY);
      thread_print_status("every 30 s");
      esp_openthread_lock_release();
    }
  }
}
