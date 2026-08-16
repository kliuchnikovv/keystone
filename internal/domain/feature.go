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

	// Environment sensing beyond temperature/humidity.
	FeatureIlluminance FeatureKey = "illuminance"
	FeaturePressure    FeatureKey = "pressure"
	FeatureFlow        FeatureKey = "flow"
	FeatureAirQuality  FeatureKey = "air_quality"
	FeatureSmoke       FeatureKey = "smoke"
	FeatureCO          FeatureKey = "co"

	// Input devices. A button reports events, not a persistent state.
	FeatureButton FeatureKey = "button"

	// Climate.
	FeatureThermostat FeatureKey = "thermostat"
	FeatureFan        FeatureKey = "fan"

	// Appliances. Mode and RunState are deliberately generic: a washer, a
	// dishwasher and a robot vacuum each expose their own Matter mode cluster,
	// but from keystone's side they are the same capability — "which program is
	// selected" and "is it running". The adapter picks the right cluster from
	// what the endpoint actually has.
	FeatureMode     FeatureKey = "mode"
	FeatureRunState FeatureKey = "run_state"

	// Media playback (TV, speaker).
	FeatureMedia FeatureKey = "media"

	// Camera control. Live video is NOT here: Matter carries only the WebRTC
	// signalling, the media itself flows over a separate peer connection.
	// Snapshots, pan/tilt/zoom and the doorbell chime are in scope.
	FeatureCamera FeatureKey = "camera"
	FeatureChime  FeatureKey = "chime"

	// EV charging.
	FeatureEVSE FeatureKey = "evse"

	// Water valves and irrigation.
	FeatureValve FeatureKey = "valve"

	// Target temperature of an appliance compartment (oven, fridge, kettle) —
	// distinct from a thermostat, which regulates a room.
	FeatureTempControl FeatureKey = "temp_control"
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

	// Colour. Matter reports colour either as CIE xy or as hue/saturation,
	// depending on the lamp's ColorMode; both are exposed rather than forcing a
	// lossy conversion on read.
	StateColorHue  StateKey = "hue"        // 0..360 for FeatureColor
	StateColorSat  StateKey = "saturation" // 0..100 for FeatureColor
	StateColorMode StateKey = "color_mode" // "xy" | "hue_sat" | "color_temp"

	// Environment.
	StateIlluminance     StateKey = "lux"          // float32 for FeatureIlluminance
	StatePressure        StateKey = "hpa"          // float32 for FeaturePressure
	StateFlow            StateKey = "m3h"          // float32 for FeatureFlow
	StateAirQualityIndex StateKey = "index"        // string enum for FeatureAirQuality
	StatePM25            StateKey = "pm25"         // float32 µg/m³
	StatePM10            StateKey = "pm10"         // float32 µg/m³
	StateCO2             StateKey = "co2"          // float32 ppm
	StateTVOC            StateKey = "tvoc"         // float32 µg/m³
	StateFormaldehyde    StateKey = "formaldehyde" // float32 µg/m³
	StateAlarm           StateKey = "alarm"        // bool for FeatureSmoke / FeatureCO

	// Input.
	StateButtonPos StateKey = "position" // int, current pressed position

	// Locks.
	StateLocked StateKey = "locked" // bool for FeatureLock

	// Climate.
	StateTargetHeat  StateKey = "target_heat" // float32 °C
	StateTargetCool  StateKey = "target_cool" // float32 °C
	StateHVACMode    StateKey = "hvac_mode"   // "off"|"heat"|"cool"|"auto"|…
	StateHVACRunning StateKey = "running"     // string, what the unit is doing now
	StateFanMode     StateKey = "fan_mode"    // "off"|"low"|"medium"|"high"|"auto"|…
	StateFanPercent  StateKey = "fan_percent" // 0..100

	// Appliances.
	StateMode      StateKey = "mode"       // int, current mode id
	StateModeLabel StateKey = "mode_label" // string, human label when known
	StateRunState  StateKey = "run_state"  // "stopped"|"running"|"paused"|"error"
	StatePhase     StateKey = "phase"      // string, current phase label
	StateCountdown StateKey = "countdown"  // seconds remaining

	// Media.
	StatePlayback StateKey = "playback" // "playing"|"paused"|"not_playing"|"buffering"

	// EV charging.
	StateEVSEState  StateKey = "evse_state"  // plug/charge state
	StateEVSESupply StateKey = "evse_supply" // supply enable state

	// Valves.
	StateValveOpen      StateKey = "open"      // bool, current state
	StateValveLevel     StateKey = "level"     // 0..100 for proportional valves
	StateValveRemaining StateKey = "remaining" // seconds left before auto-close

	// Appliance compartment temperature.
	StateSetpoint StateKey = "setpoint" // float32 °C
)

// ActionKey identifies an imperative operation within a Feature.
type ActionKey string

const (
	ActionTurnOn  ActionKey = "turn_on"
	ActionTurnOff ActionKey = "turn_off"
	ActionToggle  ActionKey = "toggle"
	ActionSet     ActionKey = "set" // generic; params carry payload

	// Locks.
	ActionLock   ActionKey = "lock"
	ActionUnlock ActionKey = "unlock"

	// Covers.
	ActionOpen  ActionKey = "open"
	ActionClose ActionKey = "close"
	ActionStop  ActionKey = "stop"

	// Appliances and media transport.
	ActionStart  ActionKey = "start"
	ActionPause  ActionKey = "pause"
	ActionResume ActionKey = "resume"
	ActionNext   ActionKey = "next"
	ActionPrev   ActionKey = "previous"

	// Camera.
	ActionSnapshot ActionKey = "snapshot"
	ActionMove     ActionKey = "move" // pan/tilt/zoom; params carry the axes
	ActionRing     ActionKey = "ring"

	// EV charging.
	ActionChargeEnable  ActionKey = "charge_enable"
	ActionChargeDisable ActionKey = "charge_disable"

	// Self-test (smoke/CO alarms).
	ActionSelfTest ActionKey = "self_test"

	// Valves reuse open/close/set above; nothing extra needed.
)

// EventKey identifies an emitted event. Distinct from state changes — e.g. a
// button press is an event without a persistent state.
type EventKey string

const (
	EventButtonPressed  EventKey = "button_pressed"
	EventMotionDetected EventKey = "motion_detected"
	EventBatteryLow     EventKey = "battery_low"

	// Buttons distinguish gestures; a controller remote is useless without them.
	EventButtonLongPress  EventKey = "button_long_press"
	EventButtonMultiPress EventKey = "button_multi_press"
	EventButtonReleased   EventKey = "button_released"

	// Alarms.
	EventSmokeAlarm EventKey = "smoke_alarm"
	EventCOAlarm    EventKey = "co_alarm"

	// Appliances.
	EventCycleComplete EventKey = "cycle_complete"

	// EV.
	EventEVConnected    EventKey = "ev_connected"
	EventEVDisconnected EventKey = "ev_disconnected"

	// Valves.
	EventValveChanged EventKey = "valve_changed"

	// EventNotConfirmed is emitted when a device accepted a command but never
	// reported the resulting state. The transport cannot tell us this: a write
	// to a read-only attribute, a command discarded because the light is off,
	// or one rejected before it left the stack all look like success. Only the
	// absence of a report gives it away.
	EventNotConfirmed EventKey = "not_confirmed"
)
