// Package domain contains the core entity types of the smart-home engine.
// Nothing in this package may import from anywhere except the Go standard
// library and small utility modules (uuid). This is the innermost layer of
// the hexagonal architecture.
package domain

import (
	"time"
)

// DeviceID uniquely identifies a device across all transports.
// Format: UUIDv7 (time-ordered) so that natural sorting yields insertion order.
type DeviceID string

// DeviceType is a high-level classification of a device — what the user thinks
// they own. It is coarse-grained: a smart plug that also measures energy is
// still a "plug", the energy metering shows up as a Feature.
type DeviceType string

const (
	DeviceTypeLight       DeviceType = "light"
	DeviceTypeSwitch      DeviceType = "switch"
	DeviceTypePlug        DeviceType = "plug"
	DeviceTypeSensor      DeviceType = "sensor"
	DeviceTypeMotion      DeviceType = "motion_sensor"
	DeviceTypeContact     DeviceType = "contact_sensor"
	DeviceTypeThermostat  DeviceType = "thermostat"
	DeviceTypeCover       DeviceType = "cover"
	DeviceTypeLock        DeviceType = "lock"
	DeviceTypeMediaPlayer DeviceType = "media_player"

	DeviceTypeFan         DeviceType = "fan"
	DeviceTypeAirPurifier DeviceType = "air_purifier"
	DeviceTypeButton      DeviceType = "button"
	DeviceTypeAlarm       DeviceType = "alarm"     // smoke / CO detectors
	DeviceTypeAppliance   DeviceType = "appliance" // washer, dishwasher, oven, fridge
	DeviceTypeVacuum      DeviceType = "vacuum"
	DeviceTypeCamera      DeviceType = "camera"
	DeviceTypeDoorbell    DeviceType = "doorbell"
	DeviceTypeEVCharger   DeviceType = "ev_charger"
	DeviceTypeWaterHeater DeviceType = "water_heater"
	DeviceTypeValve       DeviceType = "valve"
	DeviceTypeEnergyMeter DeviceType = "energy_meter"
)

// TransportKind identifies the wire protocol / integration source of a device.
type TransportKind string

const (
	TransportVirtual TransportKind = "virtual"
	TransportMatter  TransportKind = "matter"
	TransportZigbee  TransportKind = "zigbee"
	TransportLAN     TransportKind = "lan"
	TransportCloud   TransportKind = "cloud"
)

// TransportRef is the opaque identifier the transport uses for this device
// (Matter nodeID, Zigbee IEEE, etc.). Meaningless outside its
// adapter, but persisted so the adapter can re-attach after restart.
type TransportRef string

// RoomID identifies a room. Rooms are user-defined logical groupings.
type RoomID string

// Device is the top-level entity the user sees in the UI.
type Device struct {
	ID           DeviceID
	Type         DeviceType
	Name         string
	Manufacturer string
	Model        string
	Room         RoomID
	Transport    TransportKind
	TransportRef TransportRef
	Features     []Feature
	Metadata     map[string]string
	CreatedAt    time.Time
	UpdatedAt    time.Time
}

// HasFeature reports whether the device exposes a Feature of the given key.
func (d *Device) HasFeature(key FeatureKey) bool {
	for _, f := range d.Features {
		if f.Key == key {
			return true
		}
	}
	return false
}
