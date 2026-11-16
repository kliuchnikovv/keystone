// The Matter bridge on the Thread co-processor (Stage B of the F2 fork,
// docs/matter-backlog.md § 10).
//
// The hub's Matter accessory (root + aggregator) lives on this chip, so the
// bridge lives here too: every entity the brain announces becomes a bridged
// Matter endpoint, and a controller's write becomes a COMMAND back to the
// brain. The brain's side of the deal is already in place (firmware/brain-s3/
// main/main.cpp::on_bus_forward): it mirrors DECLARE / STATE / UNDECLARE of
// the Zigbee chip to this chip and forwards our COMMANDs to the Zigbee chip.
//
//   DECLARE (entity, device type, attrs) -> an endpoint with the clusters of
//       core/matter_plan.hpp (or a new one, if the entity changed shape)
//   STATE   (entity, attrs)              -> attribute::update() on that endpoint
//   UNDECLARE (entity)                   -> the endpoint is removed
//   Matter controller writes an attribute -> COMMAND to the brain
//
// The mapping (entity -> endpoint) is persisted by esp-matter's app_bridge
// machinery, so endpoint ids survive reboots and Apple Home keeps its rooms.
#pragma once

#include <cstddef>
#include <cstdint>

#include "hubproto/message.hpp"
#include "hubproto/payload.hpp"

#ifdef CHIP_HAVE_CONFIG_H
#include "esp_matter.h"
// app_bridged_device_t is a global class (esp-matter's own header), and
// esp_matter_bridge.h brings the bridge's own API (the endpoint table).
#include "app_bridged_device.h"
#include "esp_matter_bridge.h"

namespace core {
class Bus;
}  // namespace core

namespace matter_bridge {

// Bring the bridge up: the bus to the brain, the Matter node, the aggregator
// endpoint the bridged devices hang under and our boot id (it goes into every
// frame we send). Restores the endpoints of a previous run from NVS.
esp_err_t start(core::Bus* bus, esp_matter::node_t* node, std::uint16_t aggregator_ep,
                std::uint8_t boot_id);

// The hubproto frames the brain mirrors. Called from the bus handler.
void on_bus_message(const hubproto::Header& h, const std::uint8_t* payload, std::size_t len);

// The Matter controller wrote an attribute of a bridged endpoint: forward it
// to the brain as a COMMAND. Wired into esp_matter::node::create().
esp_err_t attribute_update_cb(esp_matter::attribute::callback_type_t type, std::uint16_t endpoint_id,
                              std::uint32_t cluster_id, std::uint32_t attribute_id,
                              esp_matter_attr_val_t* val, void* priv);

// A Matter controller asked the endpoint to identify itself (Apple Home's
// "Identify"). For a bridged device the blink happens on the real device, and
// this chip cannot reach it: the request goes to the brain as an IDENTIFY
// frame, which routes it to the chip that owns the entity. Returns false when
// the endpoint is not a bridged device (the hub's own root endpoint) or the
// link to the brain is down.
bool identify_endpoint(std::uint16_t endpoint_id, std::uint16_t seconds);

// esp-matter calls these when it creates or drops a bridged device.
app_bridged_device_t* alloc_bridged(esp_matter::node_t* node, std::uint16_t endpoint);
void free_bridged(app_bridged_device_t* device);

// What the bridge component believes it holds (its table lives in NVS).
void log_table();

}  // namespace matter_bridge
#endif  // CHIP_HAVE_CONFIG_H
