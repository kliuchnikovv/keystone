// Minimal Matter accessory bring-up on the Thread host.
//
// This is the opening move of docs/matter-backlog.md § 10, fork F2: the hub
// (brain-s3) stays the commissioner; the Matter server-side stack lives here
// so Apple Home / Google Home can add the hub as one accessory and see every
// hub entity as a bridged endpoint on it.
//
// Scope right now: no bridged endpoints, no bridged-device machinery, no app-
// bridge NVS table — just root endpoint + an empty aggregator (Device Type
// 0x000E) + esp_matter::start. Two reasons:
//   1. honest RAM measurement — once start() is called, CHIP singletons land
//      in .bss and the heap moves. The `heap` console command says by how much.
//   2. prove the OpenThread / Matter coexistence works at all on this build;
//      everything else (bridged clusters, forward from the hub's bus) only
//      makes sense once a controller can even see the aggregator.
//
// Only compiled when ESP_MATTER_PATH is set (scripts/matter-build.sh); a plain
// `idf.py build` without the Matter Docker image produces a plain Thread host.

#ifdef CHIP_HAVE_CONFIG_H

#include <esp_log.h>
#include <esp_matter.h>
#include <esp_matter_endpoint.h>
#include <esp_openthread_types.h>
#include <app/server/Server.h>
#include <platform/CHIPDeviceEvent.h>
#include <platform/ESP32/OpenthreadLauncher.h>

#include "core/bus.hpp"
#include "matter_bridge.hpp"

namespace {

constexpr const char* kTag = "matter_accessory";

// The Identify cluster is created by every device type the bridge builds, and
// the controller's "Identify" (Apple Home's action of that name) lands here.
// For a bridged device the blink has to happen on the physical device, which is
// on the other side of the brain: matter_bridge::identify_endpoint() sends the
// request over the bus. For the hub's own endpoint there is nothing to reach
// yet — the C6's LED is not driven from here — so it is only logged.
constexpr std::uint16_t kIdentifyFallbackSeconds = 10;

// The IdentifyTime the controller asked for (Apple Home sends 60 s). The
// callback does not carry it, so read the attribute the cluster has just set.
std::uint16_t identify_seconds(uint16_t endpoint_id) {
  esp_matter::attribute_t* attr = esp_matter::attribute::get(endpoint_id, 0x0003, 0x0000);
  esp_matter_attr_val_t v = esp_matter_invalid(nullptr);
  if (attr != nullptr && esp_matter::attribute::get_val_internal(attr, &v) == ESP_OK &&
      v.type == ESP_MATTER_VAL_TYPE_UINT16 && v.val.u16 > 0) {
    return v.val.u16;
  }
  return kIdentifyFallbackSeconds;
}

esp_err_t identification_cb(esp_matter::identification::callback_type_t type, uint16_t endpoint_id,
                            uint8_t /*effect_id*/, uint8_t /*effect_variant*/, void*) {
  // STOP arrives when IdentifyTime runs out; the device has its own timer and
  // the brain's identify is one-shot, so there is nothing to cancel.
  if (type != esp_matter::identification::START) return ESP_OK;
  if (!matter_bridge::identify_endpoint(endpoint_id, identify_seconds(endpoint_id))) {
    ESP_LOGI(kTag, "identify: endpoint %u is not a bridged device (nothing to blink)",
             static_cast<unsigned>(endpoint_id));
  }
  return ESP_OK;
}

void event_cb(const ChipDeviceEvent* event, intptr_t) {
  using chip::DeviceLayer::DeviceEventType::kCommissioningComplete;
  using chip::DeviceLayer::DeviceEventType::kCommissioningWindowOpened;
  using chip::DeviceLayer::DeviceEventType::kFabricRemoved;
  switch (event->Type) {
    case kCommissioningComplete:
      ESP_LOGI(kTag, "commissioning complete");
      break;
    case kCommissioningWindowOpened:
      ESP_LOGI(kTag, "commissioning window opened");
      break;
    case kFabricRemoved:
      if (chip::Server::GetInstance().GetFabricTable().FabricCount() == 0) {
        auto& mgr = chip::Server::GetInstance().GetCommissioningWindowManager();
        if (!mgr.IsCommissioningWindowOpen()) {
          const chip::ChipError err = mgr.OpenBasicCommissioningWindow(
              chip::System::Clock::Seconds16(300),
              chip::CommissioningWindowAdvertisement::kDnssdOnly);
          if (err != CHIP_NO_ERROR) {
            // ChipError is a class, not an enum: Format() is the sanctioned way
            // to print it (the same call CHIP's own logs use).
            ESP_LOGW(kTag, "could not open the commissioning window: %" CHIP_ERROR_FORMAT,
                     err.Format());
          }
        }
      }
      break;
    default:
      break;
  }
}

}  // namespace

// Called from main.cpp after the Thread host is up (OT started, BR on, mDNS
// hostname set). esp_matter::start will try to initialise the Thread stack
// itself; the host's init runs first, and CHIP's own `InitThreadStack` is
// expected to notice the stack is already there and attach to it. If it does
// not, the call fails and the chip keeps serving as a plain Thread host.
//
// `bus` is the link to the brain: the bridge (matter_bridge.cpp) sends its
// COMMANDs through it, and the brain mirrors the DECLARE/STATE frames of the
// Zigbee chip to us.
// Open a commissioning window so another administrator can join the hub's
// fabric: a phone in Apple Home. The accessory has no BLE (that is deliberate
// — it costs 426 KB of flash), so the phone finds it over DNS-SD (the Border
// Router reflects the SRP record onto Wi-Fi) and types the setup code.
//
// The code is the device's own — the accessory runs on the esp-matter test
// DAC (vendor 0xFFF1, product 0x8000, discriminator 3840, PIN 20202021), so
// the manual code is 3497-0112-332.
//
// The window is opened by the accessory itself and not by the brain: the
// brain's Matter worker waits for a CASE session with this chip, and while
// the chip keeps rebooting (the Pi's USB current cap) that worker never
// gets to answer.
extern "C" int matter_open_commissioning_window(std::uint16_t seconds) {
  chip::ChipError err;
  {
    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
    auto& mgr = chip::Server::GetInstance().GetCommissioningWindowManager();
    err = mgr.OpenBasicCommissioningWindow(chip::System::Clock::Seconds16(seconds),
                                           chip::CommissioningWindowAdvertisement::kDnssdOnly);
  }
  if (err != CHIP_NO_ERROR) {
    printf("could not open the commissioning window: %s\n", err.Format());
    return -1;
  }
  printf("commissioning window open for %u s\n", static_cast<unsigned>(seconds));
  printf("add the hub in Apple Home and type the setup code 3497-0112-332\n");
  return 0;
}

extern "C" void matter_accessory_start(core::Bus* bus, std::uint8_t boot_id) {
  using namespace esp_matter;

  node::config_t node_config;
  // The bridge's PRE_UPDATE hook is the one that forwards a controller's
  // write to the brain (matter_bridge.cpp).
  node_t* n = node::create(&node_config, matter_bridge::attribute_update_cb, identification_cb);
  if (n == nullptr) {
    ESP_LOGE(kTag, "node::create failed");
    return;
  }

  endpoint::aggregator::config_t agg_conf;
  endpoint_t* agg = endpoint::aggregator::create(n, &agg_conf, ENDPOINT_FLAG_NONE, nullptr);
  if (agg == nullptr) {
    ESP_LOGE(kTag, "aggregator::create failed");
    return;
  }
  ESP_LOGI(kTag, "aggregator endpoint id=%u",
           static_cast<unsigned>(endpoint::get_id(agg)));

  // CHIP wants the OpenThread platform config before it touches the stack —
  // its launcher asserts on a null pointer (OpenthreadLauncher.cpp:197).
  //
  // We are the ones who brought OpenThread up (main.cpp calls
  // esp_openthread_start()), so CHIP's own esp_openthread_init() will answer
  // ESP_ERR_INVALID_STATE ("OpenThread platform already initialized"); CHIP
  // treats that as "already initialised" and attaches to the running instance
  // instead of creating a second one. The config below is therefore only a
  // formality — but it must be valid and it must outlive the call, so it is
  // static. The port settings match ours (dataset in "nvs").
  //
  // The values are the ones main.cpp's thread_start() hands to
  // esp_openthread_start(): native radio, no host (the radio is the chip
  // itself), the dataset in "nvs". IDF 5.5 has no
  // ESP_OPENTHREAD_DEFAULT_*_CONFIG macros any more, so they are spelled out.
  static esp_openthread_platform_config_t ot_config = {
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
  };
  const esp_err_t cfg_err = set_openthread_platform_config(&ot_config);
  if (cfg_err != ESP_OK) {
    ESP_LOGE(kTag, "set_openthread_platform_config failed: %d", static_cast<int>(cfg_err));
    return;
  }

  const esp_err_t err = esp_matter::start(event_cb);
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "esp_matter::start failed: %d", static_cast<int>(err));
    return;
  }
  ESP_LOGI(kTag, "Matter accessory up");

  // The bridge: the registry entities the brain announces become bridged
  // endpoints under the aggregator (docs/matter-backlog.md § 10, Stage B).
  const esp_err_t berr = matter_bridge::start(bus, n, endpoint::get_id(agg), boot_id);
  if (berr != ESP_OK) {
    ESP_LOGE(kTag, "the Matter bridge did not come up: %d", static_cast<int>(berr));
  }
}

#else  // !CHIP_HAVE_CONFIG_H — plain Thread host build

extern "C" void matter_accessory_start(core::Bus*, std::uint8_t) {}

#endif
