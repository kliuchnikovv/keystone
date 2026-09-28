// Phase 8 Stage B (SPEC §9, matter-backlog step 1): the Thread co-processor.
//
// OpenThread comes up as a Full Thread Device on the on-chip 802.15.4 radio.
// If no dataset is stored, one is generated (random network key, PAN ID, xPAN,
// channel 15) and saved. Border-router routing to the LAN is a later stage;
// for now the C6 just forms/joins a Thread network so we can see it on air
// and, later, put Matter endpoints on top.
//
// The bus/transport half from Stage A is preserved: HELLO with chip_id=0x02,
// heartbeat every 5s, and DECLARE_END in reply to QUERY. No entities yet.
//
// Wiring (crossed, common GND): C6.TX(GPIO5) -> S3.RX(GPIO9),
//                               C6.RX(GPIO4) <- S3.TX(GPIO8).
#include <cstdint>
#include <cstring>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_openthread.h"
#include "esp_openthread_lock.h"
#include "esp_openthread_netif_glue.h"
#include "esp_openthread_types.h"
#include "esp_vfs_eventfd.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "openthread/dataset.h"
#include "openthread/dataset_ftd.h"
#include "openthread/instance.h"
#include "openthread/ip6.h"
#include "openthread/logging.h"
#include "openthread/thread.h"

#include "esp_uart_link.hpp"
#include "nvs_boot_id.hpp"
#include "core/uart_transport.hpp"
#include "hubproto/message.hpp"
#include "hubproto/payload.hpp"
#include "hubproto/version.hpp"

namespace {

const char* kTag = "thread-c6";

constexpr int kUartTxGpio = 5;
constexpr int kUartRxGpio = 4;
constexpr std::uint8_t kOwnChipId = 0x02;  // Zigbee is 0x01; Thread is 0x02.

core::UartTransport* g_bus = nullptr;
std::uint8_t g_boot_id = 0;

hubproto::Header header(hubproto::MsgType type) {
  hubproto::Header h{};
  h.version = hubproto::kProtocolVersion;
  h.type = type;
  h.chip_id = kOwnChipId;
  h.boot_id = g_boot_id;
  h.seq = 0;
  h.entity_id = hubproto::kSelfEntityId;
  return h;
}

void on_message(void* /*ctx*/, const hubproto::Header& h,
                const std::uint8_t* /*payload*/, std::size_t /*len*/) {
  if (h.type == hubproto::MsgType::Query) {
    g_bus->publish(header(hubproto::MsgType::DeclareEnd), nullptr, 0);
    ESP_LOGI(kTag, "answered QUERY with DECLARE_END (no entities yet)");
  }
}

const char* role_name(otDeviceRole r) {
  switch (r) {
    case OT_DEVICE_ROLE_DISABLED: return "disabled";
    case OT_DEVICE_ROLE_DETACHED: return "detached";
    case OT_DEVICE_ROLE_CHILD:    return "child";
    case OT_DEVICE_ROLE_ROUTER:   return "router";
    case OT_DEVICE_ROLE_LEADER:   return "leader";
    default:                       return "?";
  }
}

// Runs inside OpenThread's task once, after esp_openthread_init.
// If no dataset is committed yet, generate one; then bring IPv6 + Thread up.
void thread_bring_up(otInstance* instance) {
  if (!otDatasetIsCommissioned(instance)) {
    otOperationalDataset ds{};
    otError err = otDatasetCreateNewNetwork(instance, &ds);
    if (err != OT_ERROR_NONE) {
      ESP_LOGE(kTag, "otDatasetCreateNewNetwork failed: %d", static_cast<int>(err));
      return;
    }
    // Force a friendly network name + channel 15 (Zigbee uses 17, keep them
    // apart — see docs/hardware-wiring.md § coexistence).
    std::strncpy(ds.mNetworkName.m8, "keystone", sizeof(ds.mNetworkName.m8));
    ds.mComponents.mIsNetworkNamePresent = true;
    ds.mChannel = 15;
    ds.mComponents.mIsChannelPresent = true;
    err = otDatasetSetActive(instance, &ds);
    if (err != OT_ERROR_NONE) {
      ESP_LOGE(kTag, "otDatasetSetActive failed: %d", static_cast<int>(err));
      return;
    }
    ESP_LOGI(kTag, "Thread dataset generated (network 'keystone', channel 15)");
  }

  if (otIp6SetEnabled(instance, true) != OT_ERROR_NONE) {
    ESP_LOGW(kTag, "otIp6SetEnabled failed");
  }
  if (otThreadSetEnabled(instance, true) != OT_ERROR_NONE) {
    ESP_LOGW(kTag, "otThreadSetEnabled failed");
  }
  ESP_LOGI(kTag, "Thread stack enabled");
}

void ot_state_changed(otChangedFlags flags, void* /*ctx*/) {
  if (flags & OT_CHANGED_THREAD_ROLE) {
    otInstance* inst = esp_openthread_get_instance();
    ESP_LOGI(kTag, "Thread role now: %s (rloc16=0x%04x)",
             role_name(otThreadGetDeviceRole(inst)),
             static_cast<unsigned>(otThreadGetRloc16(inst)));
  }
}

void ot_task(void* /*arg*/) {
  // Platform config (no ESP_OPENTHREAD_DEFAULT_* macros — those live in the
  // examples' headers, not the SDK): native 802.15.4 radio, no host RCP link,
  // dataset stored in the standard NVS partition.
  esp_openthread_platform_config_t config{};
  config.radio_config.radio_mode = RADIO_MODE_NATIVE;
  config.host_config.host_connection_mode = HOST_CONNECTION_MODE_NONE;
  config.port_config.storage_partition_name = "nvs";
  config.port_config.netif_queue_size = 10;
  config.port_config.task_queue_size = 10;

  ESP_ERROR_CHECK(esp_openthread_init(&config));
  esp_openthread_lock_acquire(portMAX_DELAY);
  otInstance* instance = esp_openthread_get_instance();
  otSetStateChangedCallback(instance, ot_state_changed, nullptr);
  thread_bring_up(instance);
  esp_openthread_lock_release();

  esp_openthread_launch_mainloop();
  vTaskDelete(nullptr);
}

}  // namespace

extern "C" void app_main(void) {
  ESP_ERROR_CHECK(nvs_flash_init());
  g_boot_id = fw::next_boot_id();

  // --- UART bridge to the brain (Stage A behavior, preserved) ---
  static fw::EspUartLink link([] {
    fw::EspUartLink::Config c;
    c.port = UART_NUM_1;
    c.tx_gpio = kUartTxGpio;
    c.rx_gpio = kUartRxGpio;
    return c;
  }());
  ESP_ERROR_CHECK(link.begin());
  static core::UartTransport bus(link);
  g_bus = &bus;
  bus.subscribe(&on_message, nullptr);
  link.start();

  ESP_LOGI(kTag, "chip 0x%02x up (Thread/Matter co-processor, Stage B); boot_id=%u",
           kOwnChipId, static_cast<unsigned>(g_boot_id));

  bus.publish(header(hubproto::MsgType::Hello), nullptr, 0);

  // --- OpenThread stack ---
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  ESP_ERROR_CHECK(esp_netif_init());
  esp_vfs_eventfd_config_t eventfd_cfg = {.max_fds = 3};
  ESP_ERROR_CHECK(esp_vfs_eventfd_register(&eventfd_cfg));
  xTaskCreate(ot_task, "ot_main", 10240, nullptr, 5, nullptr);

  // Heartbeat to the brain every 5s (unchanged from Stage A).
  while (true) {
    vTaskDelay(pdMS_TO_TICKS(5000));
    bus.publish(header(hubproto::MsgType::Heartbeat), nullptr, 0);
  }
}
