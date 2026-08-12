// Package dirigera implements an Adapter for IKEA DIRIGERA hub via its
// unofficial local REST API. Communication is HTTPS on port 8443 with a
// self-signed certificate; we skip verification (LAN-only, mDNS-discovered).
//
// State updates are polled every 2s in this MVP. WebSocket streaming
// (wss://host:8443/v1) will replace polling in a follow-up.
//
// Reference implementations (unofficial, community-maintained):
//   - https://github.com/Leggin/dirigera
//   - https://github.com/mattias-lidman/dirigera-api
package dirigera

import "time"

// Device is the DIRIGERA-side representation. We keep the JSON shape close to
// what the hub returns so debugging with `curl` matches. Mapping to our
// domain.Device lives in mapping.go.
type Device struct {
	ID              string                 `json:"id"`
	Type            string                 `json:"type"`
	DeviceType      string                 `json:"deviceType"`
	CreatedAt       time.Time              `json:"createdAt"`
	IsReachable     bool                   `json:"isReachable"`
	LastSeen        time.Time              `json:"lastSeen"`
	Attributes      DeviceAttributes       `json:"attributes"`
	Capabilities    DeviceCapabilities     `json:"capabilities"`
	Room            *Room                  `json:"room,omitempty"`
	DeviceSet       []map[string]any       `json:"deviceSet"`
	RemoteLinks     []string               `json:"remoteLinks"`
	IsHidden        bool                   `json:"isHidden"`
}

// DeviceAttributes carries the union of all possible attributes. Most
// attributes are typed pointers so we can distinguish "absent" from "false".
type DeviceAttributes struct {
	CustomName   string `json:"customName,omitempty"`
	Model        string `json:"model,omitempty"`
	Manufacturer string `json:"manufacturer,omitempty"`
	FirmwareVersion string `json:"firmwareVersion,omitempty"`
	SerialNumber string `json:"serialNumber,omitempty"`

	// Light + outlet
	IsOn *bool `json:"isOn,omitempty"`

	// Light
	LightLevel        *int    `json:"lightLevel,omitempty"`        // 1..100
	ColorTemperature  *int    `json:"colorTemperature,omitempty"`  // Kelvin, 2200..4000
	ColorTemperatureMin *int  `json:"colorTemperatureMin,omitempty"`
	ColorTemperatureMax *int  `json:"colorTemperatureMax,omitempty"`
	ColorHue          *float64 `json:"colorHue,omitempty"`         // 0..360
	ColorSaturation   *float64 `json:"colorSaturation,omitempty"` // 0..1
	StartupOnOff      string  `json:"startupOnOff,omitempty"`

	// Outlet — energy monitoring (INSPELNING)
	CurrentActivePower *float64 `json:"currentActivePower,omitempty"` // Watts
	CurrentActiveEnergyMeasured *float64 `json:"currentActiveEnergyMeasured,omitempty"` // kWh
	TotalEnergyConsumed *float64 `json:"totalEnergyConsumed,omitempty"`
	TotalEnergyConsumedLastUpdated string `json:"totalEnergyConsumedLastUpdated,omitempty"`

	// Sensor — motion, contact, environment
	IsDetected           *bool    `json:"isDetected,omitempty"`         // motion / contact
	IsOpen               *bool    `json:"isOpen,omitempty"`             // open/close sensor
	BatteryPercentage    *int     `json:"batteryPercentage,omitempty"`
	CurrentTemperature   *float64 `json:"currentTemperature,omitempty"` // °C
	CurrentRH            *int     `json:"currentRH,omitempty"`          // relative humidity %
	CurrentPM25          *int     `json:"currentPM25,omitempty"`

	// Blinds
	BlindsCurrentLevel *int `json:"blindsCurrentLevel,omitempty"`
	BlindsTargetLevel  *int `json:"blindsTargetLevel,omitempty"`
	BlindsState        string `json:"blindsState,omitempty"`
}

// DeviceCapabilities lists what a device supports. Values are string lists.
type DeviceCapabilities struct {
	CanSend    []string `json:"canSend"`
	CanReceive []string `json:"canReceive"`
}

// Room is an IKEA room. We may or may not import them 1:1.
type Room struct {
	ID    string `json:"id"`
	Name  string `json:"name"`
	Color string `json:"color,omitempty"`
	Icon  string `json:"icon,omitempty"`
}

// PatchAttribute is a single-field patch body. DIRIGERA expects a JSON array
// with one object having "attributes": { ... }.
type PatchAttribute struct {
	Attributes map[string]any `json:"attributes"`
}

// ---- OAuth / pairing types ----

// AuthorizeResponse from POST /v1/oauth/authorize.
type AuthorizeResponse struct {
	Code string `json:"code"`
}

// TokenResponse from POST /v1/oauth/token.
type TokenResponse struct {
	AccessToken string `json:"access_token"`
}
