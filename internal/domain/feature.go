package domain

// Feature is a functional module of a Device — the semantic unit that a rule,
// an automation, or the UI talks to. A single RGB bulb has several features:
// onoff, brightness, color, color_temp. Inspired by Matter clusters but
// intentionally simpler and independent of any specific transport.
type Feature struct {
	Key     FeatureKey
	States  []StateKey
	Actions []ActionKey
	Events  []EventKey
	Config  map[string]any
}

// FeatureKey is the string identifier for a Feature. Standard set below;
// third-party adapters may extend with vendor-prefixed keys (e.g. "aqara.vibration").
type FeatureKey string

const (
	FeatureOnOff         FeatureKey = "onoff"
	FeatureBrightness    FeatureKey = "brightness"
	FeatureColor         FeatureKey = "color"
	FeatureColorTemp     FeatureKey = "color_temp"
	FeatureTemperature   FeatureKey = "temperature"
	FeatureHumidity      FeatureKey = "humidity"
	FeatureMotion        FeatureKey = "motion"
	FeatureContact       FeatureKey = "contact"
	FeaturePowerMeter    FeatureKey = "power_meter"
	FeatureBattery       FeatureKey = "battery"
	FeatureLock          FeatureKey = "lock"
	FeatureCoverPosition FeatureKey = "cover_position"
)

// StateKey identifies a readable attribute within a Feature.
type StateKey string

const (
	StateOnOff       StateKey = "value"     // bool for FeatureOnOff
	StateLevel       StateKey = "level"     // 0..100 for FeatureBrightness/CoverPosition
	StateColorXY     StateKey = "xy"        // [x, y] for FeatureColor
	StateColorTempK  StateKey = "kelvin"    // int for FeatureColorTemp
	StateTemperature StateKey = "celsius"   // float32 for FeatureTemperature
	StateHumidity    StateKey = "percent"   // float32 for FeatureHumidity
	StateOccupied    StateKey = "occupied"  // bool for FeatureMotion
	StateContactOpen StateKey = "open"      // bool for FeatureContact
	StatePowerNow    StateKey = "watts"     // float32 for FeaturePowerMeter
	StateEnergyTotal StateKey = "kwh_total" // float64, monotonic
	StateBatteryLvl  StateKey = "percent"   // int for FeatureBattery
)

// ActionKey identifies an imperative operation within a Feature.
type ActionKey string

const (
	ActionTurnOn  ActionKey = "turn_on"
	ActionTurnOff ActionKey = "turn_off"
	ActionToggle  ActionKey = "toggle"
	ActionSet     ActionKey = "set" // generic; params carry payload
)

// EventKey identifies an emitted event. Distinct from state changes — e.g. a
// button press is an event without a persistent state.
type EventKey string

const (
	EventButtonPressed  EventKey = "button_pressed"
	EventMotionDetected EventKey = "motion_detected"
	EventBatteryLow     EventKey = "battery_low"
)
