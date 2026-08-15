package matter

import (
	"encoding/json"
	"fmt"
	"math"
	"sort"
	"strconv"
	"strings"

	"github.com/kliuchnikovv/keystone/internal/domain"
	"github.com/kliuchnikovv/keystone/internal/ports"
)

// Matter cluster identifiers. We keep them as strings because that is what
// matter.js speaks over the wire (canonical PascalCase names from the CSA
// spec). Numeric IDs are documented for grep-ability and were taken from the
// data model matter.js ships, not typed from memory.
const (
	ClusterOnOff            = "OnOff"                       // 0x0006
	ClusterLevelControl     = "LevelControl"                // 0x0008
	ClusterColorControl     = "ColorControl"                // 0x0300
	ClusterTemperatureMeas  = "TemperatureMeasurement"      // 0x0402
	ClusterRelativeHumidity = "RelativeHumidityMeasurement" // 0x0405
	ClusterOccupancySensing = "OccupancySensing"            // 0x0406
	ClusterBooleanState     = "BooleanState"                // 0x0045
	ClusterElectricalMeas   = "ElectricalMeasurement"       // 0x0B04 (legacy, pre-1.3)
	ClusterPowerSource      = "PowerSource"                 // 0x002F

	ClusterDoorLock       = "DoorLock"               // 0x0101
	ClusterWindowCovering = "WindowCovering"         // 0x0102
	ClusterIlluminance    = "IlluminanceMeasurement" // 0x0400
	ClusterPressure       = "PressureMeasurement"    // 0x0403
	ClusterFlow           = "FlowMeasurement"        // 0x0404
	ClusterSwitch         = "Switch"                 // 0x003B
	ClusterThermostat     = "Thermostat"             // 0x0201
	ClusterFanControl     = "FanControl"             // 0x0202
	ClusterAirQuality     = "AirQuality"             // 0x005B
	ClusterSmokeCoAlarm   = "SmokeCoAlarm"           // 0x005C

	ClusterPM25         = "Pm25ConcentrationMeasurement"                          // 0x042A
	ClusterPM10         = "Pm10ConcentrationMeasurement"                          // 0x042D
	ClusterCO2          = "CarbonDioxideConcentrationMeasurement"                 // 0x040D
	ClusterTVOC         = "TotalVolatileOrganicCompoundsConcentrationMeasurement" // 0x042E
	ClusterFormaldehyde = "FormaldehydeConcentrationMeasurement"                  // 0x042B

	ClusterOperationalState    = "OperationalState"                                // 0x0060
	ClusterRvcRunMode          = "RvcRunMode"                                      // 0x0054
	ClusterRvcOperationalState = "RvcOperationalState"                             // 0x0061
	ClusterLaundryWasherMode   = "LaundryWasherMode"                               // 0x0051
	ClusterDishwasherMode      = "DishwasherMode"                                  // 0x0059
	ClusterRefrigeratorMode    = "RefrigeratorAndTemperatureControlledCabinetMode" // 0x0052
	ClusterWaterHeaterMode     = "WaterHeaterMode"                                 // 0x009E
	ClusterEvseMode            = "EnergyEvseMode"                                  // 0x009D
	ClusterTemperatureControl  = "TemperatureControl"                              // 0x0056
	ClusterModeSelect          = "ModeSelect"                                      // 0x0050
	ClusterValve               = "ValveConfigurationAndControl"                    // 0x0081

	ClusterMediaPlayback = "MediaPlayback" // 0x0506

	ClusterElectricalPower  = "ElectricalPowerMeasurement"  // 0x0090 (Matter 1.3)
	ClusterElectricalEnergy = "ElectricalEnergyMeasurement" // 0x0091 (Matter 1.3)
	ClusterEnergyEvse       = "EnergyEvse"                  // 0x0099

	ClusterCameraAvStream = "CameraAvStreamManagement"            // 0x0551
	ClusterCameraPTZ      = "CameraAvSettingsUserLevelManagement" // 0x0552
	ClusterChime          = "Chime"                               // 0x0556
)

// Matter attribute / command names.
const (
	AttrOnOff                  = "OnOff"
	AttrCurrentLevel           = "CurrentLevel"
	AttrColorTemperatureMireds = "ColorTemperatureMireds"
	AttrMeasuredValue          = "MeasuredValue"
	AttrOccupancy              = "Occupancy"
	AttrStateValue             = "StateValue"
	AttrActivePower            = "ActivePower"
	AttrTotalActiveEnergy      = "TotalActiveEnergy"
	AttrBatPercentRemaining    = "BatPercentRemaining"

	AttrCurrentX          = "CurrentX"
	AttrCurrentY          = "CurrentY"
	AttrCurrentHue        = "CurrentHue"
	AttrCurrentSaturation = "CurrentSaturation"
	AttrColorMode         = "ColorMode"

	AttrLockState = "LockState"

	AttrCurrentPositionLiftPercent100ths = "CurrentPositionLiftPercent100ths"

	AttrCurrentPosition = "CurrentPosition"

	AttrLocalTemperature        = "LocalTemperature"
	AttrOccupiedHeatingSetpoint = "OccupiedHeatingSetpoint"
	AttrOccupiedCoolingSetpoint = "OccupiedCoolingSetpoint"
	AttrSystemMode              = "SystemMode"
	AttrThermostatRunningState  = "ThermostatRunningState"

	AttrFanMode        = "FanMode"
	AttrPercentSetting = "PercentSetting"

	AttrAirQuality = "AirQuality"
	AttrSmokeState = "SmokeState"
	AttrCoState    = "CoState"

	AttrCurrentMode      = "CurrentMode"
	AttrSupportedModes   = "SupportedModes"
	AttrOperationalState = "OperationalState"
	AttrCurrentPhase     = "CurrentPhase"
	AttrCountdownTime    = "CountdownTime"

	AttrCurrentState = "CurrentState"

	AttrCumulativeEnergyImported = "CumulativeEnergyImported"
	AttrValveCurrentState        = "CurrentState"
	AttrValveCurrentLevel        = "CurrentLevel"
	AttrValveRemainingDuration   = "RemainingDuration"
	AttrTemperatureSetpoint      = "TemperatureSetpoint"

	AttrEvseState       = "State"
	AttrEvseSupplyState = "SupplyState"

	CmdOn                    = "On"
	CmdOff                   = "Off"
	CmdToggle                = "Toggle"
	CmdMoveToLevel           = "MoveToLevel"
	CmdMoveToLevelWithOnOff  = "MoveToLevelWithOnOff"
	CmdMoveToColorTempMireds = "MoveToColorTemperature"

	CmdMoveToHueAndSaturation = "MoveToHueAndSaturation"
	CmdMoveToHue              = "MoveToHue"
	CmdMoveToSaturation       = "MoveToSaturation"
	CmdMoveToColor            = "MoveToColor"

	CmdLockDoor   = "LockDoor"
	CmdUnlockDoor = "UnlockDoor"

	CmdUpOrOpen           = "UpOrOpen"
	CmdDownOrClose        = "DownOrClose"
	CmdStopMotion         = "StopMotion"
	CmdGoToLiftPercentage = "GoToLiftPercentage"

	CmdSelfTestRequest = "SelfTestRequest"

	CmdOpStart  = "Start"
	CmdOpStop   = "Stop"
	CmdOpPause  = "Pause"
	CmdOpResume = "Resume"

	CmdPlay     = "Play"
	CmdPause    = "Pause"
	CmdStop     = "Stop"
	CmdNext     = "Next"
	CmdPrevious = "Previous"

	CmdCaptureSnapshot = "CaptureSnapshot"
	CmdMptzSetPosition = "MptzSetPosition"
	CmdPlayChimeSound  = "PlayChimeSound"

	CmdEnableCharging = "EnableCharging"
	CmdEvseDisable    = "Disable"

	CmdValveOpen      = "Open"
	CmdValveClose     = "Close"
	CmdSetTemperature = "SetTemperature"
	CmdChangeToMode   = "ChangeToMode"
)

// FeatureBinding tells the adapter how to translate one keystone (Feature,
// StateKey) pair into the (cluster, attribute) Matter address.
type FeatureBinding struct {
	Cluster   string
	Attribute string
}

// featureBindings is the read/write routing table. Each (feature, state) pair
// lists candidate bindings in priority order — the adapter picks the first one
// whose cluster is actually present on the device's endpoint.
//
// Candidates exist because one keystone capability legitimately maps to several
// Matter clusters. "Which program is selected" is RvcRunMode on a vacuum,
// LaundryWasherMode on a washer and DishwasherMode on a dishwasher; power draw
// is the legacy ElectricalMeasurement on older plugs and
// ElectricalPowerMeasurement on Matter 1.3 ones. Modelling those as separate
// keystone features would push the distinction into every rule and every UI
// that only ever wanted "the mode".
//
// If a (feature, state) pair is absent, the adapter refuses the operation
// rather than guessing.
var featureBindings = map[domain.FeatureKey]map[domain.StateKey][]FeatureBinding{
	domain.FeatureOnOff: {
		domain.StateOnOff: {{Cluster: ClusterOnOff, Attribute: AttrOnOff}},
	},
	domain.FeatureBrightness: {
		domain.StateLevel: {{Cluster: ClusterLevelControl, Attribute: AttrCurrentLevel}},
	},
	domain.FeatureColorTemp: {
		domain.StateColorTempK: {{Cluster: ClusterColorControl, Attribute: AttrColorTemperatureMireds}},
	},
	domain.FeatureColor: {
		domain.StateColorHue:  {{Cluster: ClusterColorControl, Attribute: AttrCurrentHue}},
		domain.StateColorSat:  {{Cluster: ClusterColorControl, Attribute: AttrCurrentSaturation}},
		domain.StateColorMode: {{Cluster: ClusterColorControl, Attribute: AttrColorMode}},
	},
	domain.FeatureTemperature: {
		// A thermostat reports the room temperature on its own cluster; a bare
		// sensor uses TemperatureMeasurement. Same question, two answers.
		domain.StateTemperature: {
			{Cluster: ClusterTemperatureMeas, Attribute: AttrMeasuredValue},
			{Cluster: ClusterThermostat, Attribute: AttrLocalTemperature},
		},
	},
	domain.FeatureHumidity: {
		domain.StateHumidity: {{Cluster: ClusterRelativeHumidity, Attribute: AttrMeasuredValue}},
	},
	domain.FeatureMotion: {
		domain.StateOccupied: {{Cluster: ClusterOccupancySensing, Attribute: AttrOccupancy}},
	},
	domain.FeatureContact: {
		domain.StateContactOpen: {{Cluster: ClusterBooleanState, Attribute: AttrStateValue}},
	},
	domain.FeaturePowerMeter: {
		domain.StatePowerNow: {
			{Cluster: ClusterElectricalPower, Attribute: AttrActivePower},
			{Cluster: ClusterElectricalMeas, Attribute: AttrActivePower},
		},
		domain.StateEnergyTotal: {
			{Cluster: ClusterElectricalEnergy, Attribute: AttrCumulativeEnergyImported},
			{Cluster: ClusterElectricalMeas, Attribute: AttrTotalActiveEnergy},
		},
	},
	domain.FeatureBattery: {
		domain.StateBatteryLvl: {{Cluster: ClusterPowerSource, Attribute: AttrBatPercentRemaining}},
	},

	domain.FeatureIlluminance: {
		domain.StateIlluminance: {{Cluster: ClusterIlluminance, Attribute: AttrMeasuredValue}},
	},
	domain.FeaturePressure: {
		domain.StatePressure: {{Cluster: ClusterPressure, Attribute: AttrMeasuredValue}},
	},
	domain.FeatureFlow: {
		domain.StateFlow: {{Cluster: ClusterFlow, Attribute: AttrMeasuredValue}},
	},
	domain.FeatureAirQuality: {
		domain.StateAirQualityIndex: {{Cluster: ClusterAirQuality, Attribute: AttrAirQuality}},
		domain.StatePM25:            {{Cluster: ClusterPM25, Attribute: AttrMeasuredValue}},
		domain.StatePM10:            {{Cluster: ClusterPM10, Attribute: AttrMeasuredValue}},
		domain.StateCO2:             {{Cluster: ClusterCO2, Attribute: AttrMeasuredValue}},
		domain.StateTVOC:            {{Cluster: ClusterTVOC, Attribute: AttrMeasuredValue}},
		domain.StateFormaldehyde:    {{Cluster: ClusterFormaldehyde, Attribute: AttrMeasuredValue}},
	},
	domain.FeatureSmoke: {
		domain.StateAlarm: {{Cluster: ClusterSmokeCoAlarm, Attribute: AttrSmokeState}},
	},
	domain.FeatureCO: {
		domain.StateAlarm: {{Cluster: ClusterSmokeCoAlarm, Attribute: AttrCoState}},
	},

	domain.FeatureButton: {
		domain.StateButtonPos: {{Cluster: ClusterSwitch, Attribute: AttrCurrentPosition}},
	},

	domain.FeatureLock: {
		domain.StateLocked: {{Cluster: ClusterDoorLock, Attribute: AttrLockState}},
	},
	domain.FeatureCoverPosition: {
		domain.StateLevel: {{Cluster: ClusterWindowCovering, Attribute: AttrCurrentPositionLiftPercent100ths}},
	},

	domain.FeatureThermostat: {
		domain.StateTargetHeat:  {{Cluster: ClusterThermostat, Attribute: AttrOccupiedHeatingSetpoint}},
		domain.StateTargetCool:  {{Cluster: ClusterThermostat, Attribute: AttrOccupiedCoolingSetpoint}},
		domain.StateHVACMode:    {{Cluster: ClusterThermostat, Attribute: AttrSystemMode}},
		domain.StateHVACRunning: {{Cluster: ClusterThermostat, Attribute: AttrThermostatRunningState}},
	},
	domain.FeatureFan: {
		domain.StateFanMode:    {{Cluster: ClusterFanControl, Attribute: AttrFanMode}},
		domain.StateFanPercent: {{Cluster: ClusterFanControl, Attribute: AttrPercentSetting}},
	},

	domain.FeatureMode: {
		domain.StateMode: {
			{Cluster: ClusterRvcRunMode, Attribute: AttrCurrentMode},
			{Cluster: ClusterLaundryWasherMode, Attribute: AttrCurrentMode},
			{Cluster: ClusterDishwasherMode, Attribute: AttrCurrentMode},
			{Cluster: ClusterRefrigeratorMode, Attribute: AttrCurrentMode},
			{Cluster: ClusterWaterHeaterMode, Attribute: AttrCurrentMode},
			{Cluster: ClusterEvseMode, Attribute: AttrCurrentMode},
			// Generic last: a device with a specific mode cluster should use it,
			// and ModeSelect is what everything else falls back to.
			{Cluster: ClusterModeSelect, Attribute: AttrCurrentMode},
		},
	},
	domain.FeatureRunState: {
		domain.StateRunState: {
			{Cluster: ClusterRvcOperationalState, Attribute: AttrOperationalState},
			{Cluster: ClusterOperationalState, Attribute: AttrOperationalState},
		},
		domain.StatePhase: {
			{Cluster: ClusterRvcOperationalState, Attribute: AttrCurrentPhase},
			{Cluster: ClusterOperationalState, Attribute: AttrCurrentPhase},
		},
		domain.StateCountdown: {
			{Cluster: ClusterRvcOperationalState, Attribute: AttrCountdownTime},
			{Cluster: ClusterOperationalState, Attribute: AttrCountdownTime},
		},
	},

	domain.FeatureMedia: {
		domain.StatePlayback: {{Cluster: ClusterMediaPlayback, Attribute: AttrCurrentState}},
	},

	domain.FeatureValve: {
		domain.StateValveOpen:      {{Cluster: ClusterValve, Attribute: AttrValveCurrentState}},
		domain.StateValveLevel:     {{Cluster: ClusterValve, Attribute: AttrValveCurrentLevel}},
		domain.StateValveRemaining: {{Cluster: ClusterValve, Attribute: AttrValveRemainingDuration}},
	},
	domain.FeatureTempControl: {
		domain.StateSetpoint: {{Cluster: ClusterTemperatureControl, Attribute: AttrTemperatureSetpoint}},
	},

	domain.FeatureEVSE: {
		domain.StateEVSEState:  {{Cluster: ClusterEnergyEvse, Attribute: AttrEvseState}},
		domain.StateEVSESupply: {{Cluster: ClusterEnergyEvse, Attribute: AttrEvseSupplyState}},
	},
}

// writeAsCommand lists states that can only be changed through a cluster
// command, with the parameter name the corresponding ActionSet expects.
//
// Which of them are read-only is not asserted here — genAttributes carries that
// straight from the spec, and TestWriteAsCommandMatchesSpec fails if this list
// and the model ever disagree.
//
// This is not an implementation detail we can push onto callers: keystone's
// domain says "set brightness to 30", and it is the transport's job to know
// that LevelControl.CurrentLevel is `R V` and the change goes through
// MoveToLevel. Writing the attribute silently does nothing on real hardware —
// which is exactly how it failed: the lamp toggled but never dimmed.
var writeAsCommand = map[domain.FeatureKey]map[domain.StateKey]string{
	domain.FeatureBrightness:    {domain.StateLevel: "level"},
	domain.FeatureColorTemp:     {domain.StateColorTempK: "kelvin"},
	domain.FeatureColor:         {domain.StateColorHue: "hue", domain.StateColorSat: "saturation"},
	domain.FeatureCoverPosition: {domain.StateLevel: "level"},
	domain.FeatureTempControl:   {domain.StateSetpoint: "celsius"},
	domain.FeatureMode:          {domain.StateMode: "mode"},
}

// WriteAsCommand reports whether a state change has to be issued as a command,
// and under which parameter name the value travels.
func WriteAsCommand(feature domain.FeatureKey, key domain.StateKey) (string, bool) {
	param, ok := writeAsCommand[feature][key]
	return param, ok
}

// BindingFor returns the (cluster, attribute) address for a (feature, state)
// pair. clusters is what the target endpoint actually exposes; the first
// candidate present there wins. An empty list means the caller could not
// determine the endpoint's clusters, in which case the first candidate is used
// so behaviour degrades to the single-binding case rather than failing.
func BindingFor(feature domain.FeatureKey, key domain.StateKey, clusters []string) (FeatureBinding, error) {
	candidates, ok := featureBindings[feature][key]
	if !ok || len(candidates) == 0 {
		return FeatureBinding{}, fmt.Errorf("matter: no binding for feature=%s state=%s", feature, key)
	}
	if len(clusters) == 0 || len(candidates) == 1 {
		return candidates[0], nil
	}
	present := make(map[string]struct{}, len(clusters))
	for _, c := range clusters {
		present[c] = struct{}{}
	}
	for _, c := range candidates {
		if _, ok := present[c.Cluster]; ok {
			return c, nil
		}
	}
	return FeatureBinding{}, fmt.Errorf(
		"matter: feature=%s state=%s needs one of %s, endpoint exposes %v",
		feature, key, candidateClusters(candidates), clusters)
}

func candidateClusters(candidates []FeatureBinding) string {
	names := make([]string, 0, len(candidates))
	for _, c := range candidates {
		names = append(names, c.Cluster)
	}
	return strings.Join(names, "/")
}

// FeatureForCluster is the reverse map used when translating server events
// back into (feature, state) so the ingress loop can publish them on the
// internal bus. Multiple attributes on the same cluster map to different
// features (e.g. SmokeCoAlarm.SmokeState vs .CoState).
func FeatureForCluster(cluster, attribute string) (domain.FeatureKey, domain.StateKey, bool) {
	for feature, states := range featureBindings {
		for state, candidates := range states {
			for _, b := range candidates {
				if b.Cluster == cluster && b.Attribute == attribute {
					return feature, state, true
				}
			}
		}
	}
	return "", "", false
}

// --- Value conversions ---
//
// Matter uses transport-specific numeric encodings that we normalise into the
// units keystone Features expose.

// LevelToPercent maps Matter LevelControl (0..254) to keystone brightness 0..100.
func LevelToPercent(level int) int {
	if level < 0 {
		return 0
	}
	if level > 254 {
		return 100
	}
	return int(float64(level) * 100.0 / 254.0)
}

// PercentToLevel is the inverse — used when writing brightness back.
func PercentToLevel(percent int) int {
	if percent < 0 {
		return 0
	}
	if percent > 100 {
		return 254
	}
	return int(float64(percent) * 254.0 / 100.0)
}

// MiredsToKelvin converts Matter's mireds (micro-reciprocal-degrees) into
// Kelvin, which is what keystone stores. mireds = 1_000_000 / kelvin, with
// rounding to nearest so the K → mireds → K round-trip stays within ±10K
// across the useful 2000–7000K range.
func MiredsToKelvin(mireds int) int {
	if mireds <= 0 {
		return 0
	}
	return (1_000_000 + mireds/2) / mireds
}

// KelvinToMireds is the inverse.
func KelvinToMireds(kelvin int) int {
	if kelvin <= 0 {
		return 0
	}
	return (1_000_000 + kelvin/2) / kelvin
}

// CentiCelsius normalises Matter TemperatureMeasurement.MeasuredValue
// (int16 in 0.01°C) into float32 Celsius.
func CentiCelsius(raw int) float32 {
	return float32(raw) / 100.0
}

// CentiPercent normalises RelativeHumidityMeasurement.MeasuredValue
// (uint16 in 0.01%) into float32 percent.
func CentiPercent(raw int) float32 {
	return float32(raw) / 100.0
}

// DegreesToHue maps 0..360° onto Matter's 0..254 hue range, and back.
func DegreesToHue(deg int) int {
	if deg < 0 {
		deg = ((deg % 360) + 360) % 360
	}
	if deg >= 360 {
		deg %= 360
	}
	return deg * 254 / 360
}

func HueToDegrees(hue int) int {
	if hue < 0 {
		return 0
	}
	if hue > 254 {
		hue = 254
	}
	return hue * 360 / 254
}

// PercentToSaturation / SaturationToPercent convert 0..100 to Matter's 0..254.
func PercentToSaturation(pct int) int { return clampPercent(pct) * 254 / 100 }
func SaturationToPercent(sat int) int {
	if sat < 0 {
		return 0
	}
	if sat > 254 {
		return 100
	}
	return sat * 100 / 254
}

// CieToMatter / MatterToCie convert CIE xy (0..1) to the uint16 fixed-point
// encoding ColorControl uses (1/65536 steps).
func CieToMatter(v float64) int {
	if v < 0 {
		v = 0
	}
	if v > 1 {
		v = 1
	}
	return int(v * 65536)
}

func MatterToCie(raw int) float64 { return float64(raw) / 65536.0 }

// PercentToHundredths / HundredthsToPercent convert between keystone percent
// and the hundredths-of-a-percent WindowCovering speaks.
func PercentToHundredths(pct int) int { return clampPercent(pct) * 100 }
func HundredthsToPercent(raw int) int {
	if raw < 0 {
		return 0
	}
	if raw > 10000 {
		return 100
	}
	return raw / 100
}

func clampPercent(pct int) int {
	if pct < 0 {
		return 0
	}
	if pct > 100 {
		return 100
	}
	return pct
}

// LogLuxToLux inverts Matter's IlluminanceMeasurement encoding, which stores
// 10000 * log10(lux) + 1 so that a huge dynamic range fits in a uint16. Zero
// means "unknown", not "dark".
func LogLuxToLux(raw int) float32 {
	if raw <= 0 {
		return 0
	}
	return float32(math.Pow(10, (float64(raw)-1)/10000))
}

// --- enum decoding ---
//
// matter.js delivers enums either as numbers or as their spec names, depending
// on whether the cluster is modelled. Both are accepted and normalised to the
// lowercase strings keystone stores, so rules can compare against stable words
// rather than magic numbers.

// Таблицы значений берутся из сгенерированного matter_gen.go: раньше они
// набивались по памяти, и в двух из восьми были пропуски — у робота-пылесоса
// не хватало половины состояний, у зарядки авто одного.
var (
	lockStates        = genEnums["DoorLock.LockStateEnum"]
	hvacModes         = genEnums["Thermostat.SystemModeEnum"]
	fanModes          = genEnums["FanControl.FanModeEnum"]
	airQualityLevels  = genEnums["AirQuality.AirQualityEnum"]
	alarmStates       = genEnums["SmokeCoAlarm.AlarmStateEnum"]
	operationalStates = genEnums["RvcOperationalState.OperationalStateEnum"]
	playbackStates    = genEnums["MediaPlayback.PlaybackStateEnum"]
	evseStates        = genEnums["EnergyEvse.StateEnum"]
	evseSupplyStates  = genEnums["EnergyEvse.SupplyStateEnum"]
	colorModes        = genEnums["ColorControl.ColorModeEnum"]
	valveStates       = genEnums["ValveConfigurationAndControl.ValveStateEnum"]
)

// enumValue normalises a Matter enum to a keystone string. Names that arrive
// already decoded are lower-cased; unknown numbers are surfaced as-is rather
// than silently becoming "unknown", so a spec addition is visible instead of
// masked.
func enumValue(v any, table map[int]string) (string, error) {
	if s, ok := v.(string); ok {
		return toSnake(s), nil
	}
	n, err := asInt(v)
	if err != nil {
		return "", err
	}
	if name, ok := table[n]; ok {
		return name, nil
	}
	return strconv.Itoa(n), nil
}

// toSnake converts matter.js enum names ("NotFullyLocked") to keystone's
// lowercase form ("not_fully_locked").
func toSnake(s string) string {
	var b strings.Builder
	for i, r := range s {
		if r >= 'A' && r <= 'Z' {
			if i > 0 {
				b.WriteByte('_')
			}
			b.WriteRune(r - 'A' + 'a')
			continue
		}
		b.WriteRune(r)
	}
	return b.String()
}

// --- Node → DiscoveredDevice ---

// nodeToDiscovered turns one Matter node reported by listNodes into the
// DiscoveredDevice keystone shows to the user. Features are inferred from
// the clusters present on the first functional endpoint; DeviceType is
// inferred from the feature set.
func nodeToDiscovered(n Node) (ports.DiscoveredDevice, featureRoutes) {
	features, routes := featuresFromNode(n)
	meta := map[string]string{"matter.nodeId": n.NodeID}
	for feature, endpoint := range routes {
		meta["matter.endpoint."+string(feature)] = strconv.Itoa(endpoint)
	}
	return ports.DiscoveredDevice{
		TransportRef: domain.TransportRef(n.NodeID),
		Type:         deviceTypeFor(n, features),
		Name:         displayNameFor(n),
		Manufacturer: n.VendorName,
		Model:        n.ProductName,
		Features:     features,
		Metadata:     meta,
	}, routes
}

func displayNameFor(n Node) string {
	switch {
	// NodeLabel is what the user typed in Apple Home / Google Home, so it beats
	// anything we can assemble from the vendor's own strings.
	case n.NodeLabel != "":
		return n.NodeLabel
	case n.ProductName != "" && n.VendorName != "":
		return n.VendorName + " " + n.ProductName
	case n.ProductName != "":
		return n.ProductName
	case n.VendorName != "":
		return n.VendorName
	default:
		return "Matter device " + n.NodeID
	}
}

// matterDeviceTypes maps canonical CSA device-type names onto keystone's
// coarse DeviceType. Only types keystone has a category for are listed;
// anything else falls through to the feature heuristic.
//
// This is the authoritative signal: a plug and a light both expose nothing but
// OnOff, and no amount of cluster inspection tells them apart.
var matterDeviceTypes = map[string]domain.DeviceType{
	"OnOffLight":                 domain.DeviceTypeLight,
	"DimmableLight":              domain.DeviceTypeLight,
	"ColorTemperatureLight":      domain.DeviceTypeLight,
	"ExtendedColorLight":         domain.DeviceTypeLight,
	"MountedDimmableLoadControl": domain.DeviceTypeLight,

	"OnOffPlugInUnit":     domain.DeviceTypePlug,
	"DimmablePlugInUnit":  domain.DeviceTypePlug,
	"MountedOnOffControl": domain.DeviceTypePlug,

	"OnOffLightSwitch":  domain.DeviceTypeSwitch,
	"DimmerSwitch":      domain.DeviceTypeSwitch,
	"ColorDimmerSwitch": domain.DeviceTypeSwitch,
	"ControlBridge":     domain.DeviceTypeSwitch,
	"GenericSwitch":     domain.DeviceTypeButton,

	"ContactSensor":   domain.DeviceTypeContact,
	"OccupancySensor": domain.DeviceTypeMotion,

	"TemperatureSensor":   domain.DeviceTypeSensor,
	"HumiditySensor":      domain.DeviceTypeSensor,
	"LightSensor":         domain.DeviceTypeSensor,
	"PressureSensor":      domain.DeviceTypeSensor,
	"FlowSensor":          domain.DeviceTypeSensor,
	"AirQualitySensor":    domain.DeviceTypeSensor,
	"SoilSensor":          domain.DeviceTypeSensor,
	"RainSensor":          domain.DeviceTypeSensor,
	"SmokeCoAlarm":        domain.DeviceTypeAlarm,
	"WaterLeakDetector":   domain.DeviceTypeSensor,
	"WaterFreezeDetector": domain.DeviceTypeSensor,
	"OnOffSensor":         domain.DeviceTypeSensor,

	"Thermostat":               domain.DeviceTypeThermostat,
	"ThermostatController":     domain.DeviceTypeThermostat,
	"RoomAirConditioner":       domain.DeviceTypeThermostat,
	"HeatPump":                 domain.DeviceTypeThermostat,
	"WindowCovering":           domain.DeviceTypeCover,
	"WindowCoveringController": domain.DeviceTypeCover,
	"Closure":                  domain.DeviceTypeCover,
	"DoorLock":                 domain.DeviceTypeLock,
	"DoorLockController":       domain.DeviceTypeLock,

	"Fan":           domain.DeviceTypeFan,
	"AirPurifier":   domain.DeviceTypeAirPurifier,
	"ExtractorHood": domain.DeviceTypeFan,

	"RoboticVacuumCleaner": domain.DeviceTypeVacuum,

	"LaundryWasher":                domain.DeviceTypeAppliance,
	"LaundryDryer":                 domain.DeviceTypeAppliance,
	"Dishwasher":                   domain.DeviceTypeAppliance,
	"Refrigerator":                 domain.DeviceTypeAppliance,
	"Oven":                         domain.DeviceTypeAppliance,
	"MicrowaveOven":                domain.DeviceTypeAppliance,
	"Cooktop":                      domain.DeviceTypeAppliance,
	"CookSurface":                  domain.DeviceTypeAppliance,
	"TemperatureControlledCabinet": domain.DeviceTypeAppliance,

	"WaterHeater":      domain.DeviceTypeWaterHeater,
	"WaterValve":       domain.DeviceTypeValve,
	"IrrigationSystem": domain.DeviceTypeValve,
	"Pump":             domain.DeviceTypeValve,
	"PumpController":   domain.DeviceTypeValve,

	"EnergyEvse":             domain.DeviceTypeEVCharger,
	"ElectricalMeter":        domain.DeviceTypeEnergyMeter,
	"ElectricalUtilityMeter": domain.DeviceTypeEnergyMeter,
	"BatteryStorage":         domain.DeviceTypeEnergyMeter,
	"SolarPower":             domain.DeviceTypeEnergyMeter,
	"DeviceEnergyManagement": domain.DeviceTypeEnergyMeter,

	"Camera":           domain.DeviceTypeCamera,
	"FloodlightCamera": domain.DeviceTypeCamera,
	"SnapshotCamera":   domain.DeviceTypeCamera,
	"Doorbell":         domain.DeviceTypeDoorbell,
	"VideoDoorbell":    domain.DeviceTypeDoorbell,
	"AudioDoorbell":    domain.DeviceTypeDoorbell,
	"Chime":            domain.DeviceTypeDoorbell,

	"Speaker":            domain.DeviceTypeMediaPlayer,
	"ContentApp":         domain.DeviceTypeMediaPlayer,
	"CastingVideoClient": domain.DeviceTypeMediaPlayer,
	"BasicVideoPlayer":   domain.DeviceTypeMediaPlayer,
	"CastingVideoPlayer": domain.DeviceTypeMediaPlayer,

	// Infrastructure endpoints — never the device's own identity. Listed so
	// deviceTypeFor skips them instead of latching onto the first endpoint.
	"RootNode":         "",
	"Aggregator":       "",
	"BridgedNode":      "",
	"PowerSource":      "",
	"OtaRequestor":     "",
	"OtaProvider":      "",
	"ElectricalSensor": "",
}

// featureRoutes records which endpoint each Feature actually lives on, so
// reads, writes and commands address the right one. Without it every operation
// went to endpoint 1, which silently does the wrong thing on any device with
// more than one function (a two-socket plug, a light strip with segments).
type featureRoutes map[domain.FeatureKey]int

// nodeRoutes is everything the adapter needs to address one node: which
// endpoint each feature lives on, and which clusters that endpoint exposes.
// The cluster set is what lets BindingFor choose between candidates — a washer
// and a vacuum both have a "mode", carried by different clusters.
type nodeRoutes struct {
	features featureRoutes
	clusters map[int][]string
}

// clustersFromNode indexes a node's clusters by endpoint.
func clustersFromNode(n Node) map[int][]string {
	out := make(map[int][]string, len(n.Endpoints))
	for _, ep := range n.Endpoints {
		out[ep.EndpointID] = ep.Clusters
	}
	return out
}

// featuresFromNode collects the union of Features present on any endpoint,
// along with the endpoint each one came from. Endpoints are visited in the
// order the sidecar reported them (ascending), so for a feature exposed on
// several endpoints the lowest wins — matching the old defaultEndpoint
// behaviour for simple devices while giving multi-endpoint devices a real
// address for everything else.
//
// Endpoint 0 is skipped for feature purposes: it is the node's administrative
// root (BasicInformation, OperationalCredentials), never a function the user
// controls.
func featuresFromNode(n Node) ([]domain.Feature, featureRoutes) {
	index := map[domain.FeatureKey]int{}
	routes := featureRoutes{}
	var out []domain.Feature

	for _, ep := range n.Endpoints {
		if ep.EndpointID == 0 {
			continue
		}
		for _, cluster := range ep.Clusters {
			for _, f := range featuresForCluster(cluster) {
				pos, seen := index[f.Key]
				if !seen {
					index[f.Key] = len(out)
					routes[f.Key] = ep.EndpointID
					out = append(out, f)
					continue
				}
				// The same capability can be assembled from several clusters:
				// air quality is one feature spread over PM2.5, PM10, CO2 and
				// TVOC clusters, and a thermostat contributes the room
				// temperature alongside a bare sensor. Merge instead of
				// dropping, or all but the first cluster's states vanish.
				mergeFeature(&out[pos], f)
			}
		}
	}
	return out, routes
}

// mergeFeature folds src into dst, keeping each state/action/event once and
// preserving first-seen order.
func mergeFeature(dst *domain.Feature, src domain.Feature) {
	dst.States = mergeKeys(dst.States, src.States)
	dst.Actions = mergeKeys(dst.Actions, src.Actions)
	dst.Events = mergeKeys(dst.Events, src.Events)
}

func mergeKeys[T comparable](dst, src []T) []T {
	if len(src) == 0 {
		return dst
	}
	seen := make(map[T]struct{}, len(dst)+len(src))
	for _, v := range dst {
		seen[v] = struct{}{}
	}
	for _, v := range src {
		if _, ok := seen[v]; ok {
			continue
		}
		seen[v] = struct{}{}
		dst = append(dst, v)
	}
	return dst
}

// featuresForCluster is the cluster → Feature(s) fan-out used during
// discovery. Kept independent of featureBindings so that a cluster which
// contributes multiple attributes (Electrical: ActivePower + Energy) surfaces
// as one keystone Feature.
func featuresForCluster(cluster string) []domain.Feature {
	switch cluster {
	case ClusterOnOff:
		return []domain.Feature{{
			Key:     domain.FeatureOnOff,
			States:  []domain.StateKey{domain.StateOnOff},
			Actions: []domain.ActionKey{domain.ActionTurnOn, domain.ActionTurnOff, domain.ActionToggle},
		}}
	case ClusterLevelControl:
		return []domain.Feature{{
			Key:     domain.FeatureBrightness,
			States:  []domain.StateKey{domain.StateLevel},
			Actions: []domain.ActionKey{domain.ActionSet},
		}}
	case ClusterColorControl:
		// One cluster, two keystone features: colour temperature and colour are
		// separate controls in every UI, and plenty of lamps support only the
		// former.
		return []domain.Feature{
			{
				Key:     domain.FeatureColorTemp,
				States:  []domain.StateKey{domain.StateColorTempK},
				Actions: []domain.ActionKey{domain.ActionSet},
			},
			{
				Key:     domain.FeatureColor,
				States:  []domain.StateKey{domain.StateColorHue, domain.StateColorSat, domain.StateColorMode},
				Actions: []domain.ActionKey{domain.ActionSet},
			},
		}
	case ClusterTemperatureMeas:
		return []domain.Feature{{Key: domain.FeatureTemperature, States: []domain.StateKey{domain.StateTemperature}}}
	case ClusterRelativeHumidity:
		return []domain.Feature{{Key: domain.FeatureHumidity, States: []domain.StateKey{domain.StateHumidity}}}
	case ClusterOccupancySensing:
		return []domain.Feature{{Key: domain.FeatureMotion, States: []domain.StateKey{domain.StateOccupied}}}
	case ClusterBooleanState:
		return []domain.Feature{{Key: domain.FeatureContact, States: []domain.StateKey{domain.StateContactOpen}}}
	case ClusterElectricalMeas, ClusterElectricalPower, ClusterElectricalEnergy:
		return []domain.Feature{{Key: domain.FeaturePowerMeter, States: []domain.StateKey{domain.StatePowerNow, domain.StateEnergyTotal}}}
	case ClusterPowerSource:
		return []domain.Feature{{Key: domain.FeatureBattery, States: []domain.StateKey{domain.StateBatteryLvl}}}

	case ClusterIlluminance:
		return []domain.Feature{{Key: domain.FeatureIlluminance, States: []domain.StateKey{domain.StateIlluminance}}}
	case ClusterPressure:
		return []domain.Feature{{Key: domain.FeaturePressure, States: []domain.StateKey{domain.StatePressure}}}
	case ClusterFlow:
		return []domain.Feature{{Key: domain.FeatureFlow, States: []domain.StateKey{domain.StateFlow}}}

	case ClusterAirQuality:
		return []domain.Feature{{Key: domain.FeatureAirQuality, States: []domain.StateKey{domain.StateAirQualityIndex}}}
	case ClusterPM25:
		return []domain.Feature{{Key: domain.FeatureAirQuality, States: []domain.StateKey{domain.StatePM25}}}
	case ClusterPM10:
		return []domain.Feature{{Key: domain.FeatureAirQuality, States: []domain.StateKey{domain.StatePM10}}}
	case ClusterCO2:
		return []domain.Feature{{Key: domain.FeatureAirQuality, States: []domain.StateKey{domain.StateCO2}}}
	case ClusterTVOC:
		return []domain.Feature{{Key: domain.FeatureAirQuality, States: []domain.StateKey{domain.StateTVOC}}}
	case ClusterFormaldehyde:
		return []domain.Feature{{Key: domain.FeatureAirQuality, States: []domain.StateKey{domain.StateFormaldehyde}}}

	case ClusterSmokeCoAlarm:
		// Smoke and CO are separate alarms on one cluster; a rule that reacts to
		// smoke must not fire on a CO reading.
		return []domain.Feature{
			{
				Key:     domain.FeatureSmoke,
				States:  []domain.StateKey{domain.StateAlarm},
				Events:  []domain.EventKey{domain.EventSmokeAlarm},
				Actions: []domain.ActionKey{domain.ActionSelfTest},
			},
			{
				Key:    domain.FeatureCO,
				States: []domain.StateKey{domain.StateAlarm},
				Events: []domain.EventKey{domain.EventCOAlarm},
			},
		}

	case ClusterSwitch:
		return []domain.Feature{{
			Key:    domain.FeatureButton,
			States: []domain.StateKey{domain.StateButtonPos},
			Events: []domain.EventKey{
				domain.EventButtonPressed,
				domain.EventButtonReleased,
				domain.EventButtonLongPress,
				domain.EventButtonMultiPress,
			},
		}}

	case ClusterDoorLock:
		return []domain.Feature{{
			Key:     domain.FeatureLock,
			States:  []domain.StateKey{domain.StateLocked},
			Actions: []domain.ActionKey{domain.ActionLock, domain.ActionUnlock},
		}}
	case ClusterWindowCovering:
		return []domain.Feature{{
			Key:    domain.FeatureCoverPosition,
			States: []domain.StateKey{domain.StateLevel},
			Actions: []domain.ActionKey{
				domain.ActionOpen, domain.ActionClose, domain.ActionStop, domain.ActionSet,
			},
		}}

	case ClusterThermostat:
		return []domain.Feature{
			{
				Key: domain.FeatureThermostat,
				States: []domain.StateKey{
					domain.StateTargetHeat, domain.StateTargetCool,
					domain.StateHVACMode, domain.StateHVACRunning,
				},
				Actions: []domain.ActionKey{domain.ActionSet},
			},
			// A thermostat also knows the room temperature; exposing it as the
			// ordinary temperature feature means rules do not special-case it.
			{Key: domain.FeatureTemperature, States: []domain.StateKey{domain.StateTemperature}},
		}
	case ClusterFanControl:
		return []domain.Feature{{
			Key:     domain.FeatureFan,
			States:  []domain.StateKey{domain.StateFanMode, domain.StateFanPercent},
			Actions: []domain.ActionKey{domain.ActionSet},
		}}

	case ClusterValve:
		return []domain.Feature{{
			Key:    domain.FeatureValve,
			States: []domain.StateKey{domain.StateValveOpen, domain.StateValveLevel, domain.StateValveRemaining},
			Actions: []domain.ActionKey{
				domain.ActionOpen, domain.ActionClose, domain.ActionSet,
			},
		}}
	case ClusterTemperatureControl:
		return []domain.Feature{{
			Key:     domain.FeatureTempControl,
			States:  []domain.StateKey{domain.StateSetpoint},
			Actions: []domain.ActionKey{domain.ActionSet},
		}}

	case ClusterRvcRunMode, ClusterLaundryWasherMode, ClusterDishwasherMode,
		ClusterRefrigeratorMode, ClusterWaterHeaterMode, ClusterEvseMode, ClusterModeSelect:
		return []domain.Feature{{
			Key:     domain.FeatureMode,
			States:  []domain.StateKey{domain.StateMode},
			Actions: []domain.ActionKey{domain.ActionSet},
		}}
	case ClusterOperationalState, ClusterRvcOperationalState:
		return []domain.Feature{{
			Key:     domain.FeatureRunState,
			States:  []domain.StateKey{domain.StateRunState, domain.StatePhase, domain.StateCountdown},
			Events:  []domain.EventKey{domain.EventCycleComplete},
			Actions: []domain.ActionKey{domain.ActionStart, domain.ActionStop, domain.ActionPause, domain.ActionResume},
		}}

	case ClusterMediaPlayback:
		return []domain.Feature{{
			Key:    domain.FeatureMedia,
			States: []domain.StateKey{domain.StatePlayback},
			Actions: []domain.ActionKey{
				domain.ActionStart, domain.ActionPause, domain.ActionStop,
				domain.ActionNext, domain.ActionPrev,
			},
		}}

	case ClusterEnergyEvse:
		return []domain.Feature{{
			Key:     domain.FeatureEVSE,
			States:  []domain.StateKey{domain.StateEVSEState, domain.StateEVSESupply},
			Events:  []domain.EventKey{domain.EventEVConnected, domain.EventEVDisconnected},
			Actions: []domain.ActionKey{domain.ActionChargeEnable, domain.ActionChargeDisable},
		}}

	case ClusterCameraAvStream:
		// Stills only. Live video is negotiated over Matter but carried by a
		// separate WebRTC connection, which keystone does not terminate.
		return []domain.Feature{{
			Key:     domain.FeatureCamera,
			Actions: []domain.ActionKey{domain.ActionSnapshot},
		}}
	case ClusterCameraPTZ:
		return []domain.Feature{{
			Key:     domain.FeatureCamera,
			Actions: []domain.ActionKey{domain.ActionMove},
		}}
	case ClusterChime:
		return []domain.Feature{{
			Key:     domain.FeatureChime,
			Actions: []domain.ActionKey{domain.ActionRing},
		}}

	default:
		return nil
	}
}

// deviceTypeFor classifies a node. The endpoints' declared Matter device types
// are authoritative and tried first; the feature heuristic is the fallback for
// endpoints that haven't been interviewed yet or declare a type outside
// keystone's vocabulary.
func deviceTypeFor(n Node, features []domain.Feature) domain.DeviceType {
	for _, ep := range n.Endpoints {
		if ep.EndpointID == 0 || ep.DeviceType == "" {
			continue
		}
		// A known-but-infrastructural type maps to "" and is skipped, so a
		// bridge's Aggregator endpoint doesn't shadow the real device behind it.
		if t, ok := matterDeviceTypes[ep.DeviceType]; ok && t != "" {
			return t
		}
	}
	return deviceTypeFromFeatures(features)
}

// deviceTypeFromFeatures guesses the coarse-grained DeviceType from the feature
// set. A device with color/dimming is a light; only OnOff is a switch/plug (we
// lean on switch as safer default — plugs typically also carry power metering).
func deviceTypeFromFeatures(features []domain.Feature) domain.DeviceType {
	has := map[domain.FeatureKey]bool{}
	for _, f := range features {
		has[f.Key] = true
	}
	switch {
	case has[domain.FeatureBrightness] || has[domain.FeatureColorTemp] || has[domain.FeatureColor]:
		return domain.DeviceTypeLight
	case has[domain.FeatureMotion]:
		return domain.DeviceTypeMotion
	case has[domain.FeatureContact]:
		return domain.DeviceTypeContact
	case has[domain.FeatureTemperature] || has[domain.FeatureHumidity]:
		return domain.DeviceTypeSensor
	case has[domain.FeaturePowerMeter]:
		return domain.DeviceTypePlug
	case has[domain.FeatureOnOff]:
		return domain.DeviceTypeSwitch
	default:
		return domain.DeviceTypeSensor
	}
}

// commissionableFrom converts one advertisement into the port type. The
// advertised device type is mapped through the same table as commissioned
// devices, so a lamp reads as a lamp before it is ever paired; unknown or
// unadvertised types stay empty rather than being guessed from nothing.
func commissionableFrom(d CommissionableDevice) ports.CommissionableDevice {
	out := ports.CommissionableDevice{
		Ref:           d.Ref,
		Name:          d.Name,
		VendorID:      d.VendorID,
		ProductID:     d.ProductID,
		Discriminator: d.Discriminator,
	}
	if t, ok := matterDeviceTypes[d.DeviceType]; ok && t != "" {
		out.Type = t
	}
	if out.Name == "" {
		// Something has to identify the row in a picker. The discriminator is
		// printed next to the QR code on most devices, so it is the one value a
		// user can actually match against the thing in their hand.
		if d.Discriminator != 0 {
			out.Name = "Matter device " + strconv.Itoa(d.Discriminator)
		} else {
			out.Name = "Matter device"
		}
	}
	return out
}

// matterEvents maps a (cluster, event) pair onto the keystone event a rule can
// trigger on. Only events that mean something to a user are listed: a rule
// reacts to "the button was double-pressed", never to "MultiPressOngoing".
var matterEvents = map[string]map[string]domain.EventKey{
	ClusterSwitch: {
		"InitialPress":       domain.EventButtonPressed,
		"ShortRelease":       domain.EventButtonReleased,
		"LongPress":          domain.EventButtonLongPress,
		"MultiPressComplete": domain.EventButtonMultiPress,
	},
	ClusterSmokeCoAlarm: {
		"SmokeAlarm": domain.EventSmokeAlarm,
		"CoAlarm":    domain.EventCOAlarm,
		"LowBattery": domain.EventBatteryLow,
	},
	ClusterOperationalState: {
		"OperationCompletion": domain.EventCycleComplete,
	},
	ClusterRvcOperationalState: {
		"OperationCompletion": domain.EventCycleComplete,
	},
	ClusterEnergyEvse: {
		"EvConnected":   domain.EventEVConnected,
		"EvNotDetected": domain.EventEVDisconnected,
	},
	ClusterOccupancySensing: {
		"OccupancyChanged": domain.EventMotionDetected,
	},
	ClusterValve: {
		"ValveStateChanged": domain.EventValveChanged,
	},
}

// eventFeatures says which feature owns each cluster's events, so a published
// event carries the same feature key the device advertises.
var eventFeatures = map[string]domain.FeatureKey{
	ClusterSwitch:              domain.FeatureButton,
	ClusterOperationalState:    domain.FeatureRunState,
	ClusterRvcOperationalState: domain.FeatureRunState,
	ClusterEnergyEvse:          domain.FeatureEVSE,
	ClusterOccupancySensing:    domain.FeatureMotion,
	ClusterValve:               domain.FeatureValve,
}

// EventForCluster resolves a Matter event to its keystone (feature, event)
// pair. SmokeCoAlarm needs the event name to pick between the smoke and CO
// features, which share the cluster.
func EventForCluster(cluster, event string) (domain.FeatureKey, domain.EventKey, bool) {
	key, ok := matterEvents[cluster][event]
	if !ok {
		return "", "", false
	}
	if cluster == ClusterSmokeCoAlarm {
		switch key {
		case domain.EventCOAlarm:
			return domain.FeatureCO, key, true
		default:
			return domain.FeatureSmoke, key, true
		}
	}
	feature, ok := eventFeatures[cluster]
	if !ok {
		return "", "", false
	}
	return feature, key, true
}

// --- attribute encode / decode ---

// decodeAttribute parses the raw JSON returned by readAttribute into the
// typed keystone value for the requested (feature, state) pair.
func decodeAttribute(feature domain.FeatureKey, key domain.StateKey, raw json.RawMessage) (any, error) {
	var v any
	if err := json.Unmarshal(raw, &v); err != nil {
		return nil, fmt.Errorf("matter: parse attribute json: %w", err)
	}
	return decodeAttributeAny(feature, key, v)
}

// decodeAttributeAny is decodeAttribute for the case where the value was
// already unmarshaled once (e.g. an attributeChanged event payload).
func decodeAttributeAny(feature domain.FeatureKey, key domain.StateKey, v any) (any, error) {
	switch {
	case feature == domain.FeatureOnOff && key == domain.StateOnOff:
		b, ok := v.(bool)
		if !ok {
			return nil, typeMismatch("bool", v)
		}
		return b, nil
	case feature == domain.FeatureBrightness && key == domain.StateLevel:
		n, err := asInt(v)
		if err != nil {
			return nil, err
		}
		return LevelToPercent(n), nil
	case feature == domain.FeatureColorTemp && key == domain.StateColorTempK:
		n, err := asInt(v)
		if err != nil {
			return nil, err
		}
		return MiredsToKelvin(n), nil
	case feature == domain.FeatureTemperature && key == domain.StateTemperature:
		n, err := asInt(v)
		if err != nil {
			return nil, err
		}
		return CentiCelsius(n), nil
	case feature == domain.FeatureHumidity && key == domain.StateHumidity:
		n, err := asInt(v)
		if err != nil {
			return nil, err
		}
		return CentiPercent(n), nil
	case feature == domain.FeatureMotion && key == domain.StateOccupied:
		// Matter Occupancy is a bitmap; low bit == occupied.
		n, err := asInt(v)
		if err != nil {
			// Also accept plain bool for lenient sidecars.
			if b, ok := v.(bool); ok {
				return b, nil
			}
			return nil, err
		}
		return n&0x01 == 0x01, nil
	case feature == domain.FeatureContact && key == domain.StateContactOpen:
		b, ok := v.(bool)
		if !ok {
			return nil, typeMismatch("bool", v)
		}
		return b, nil
	case feature == domain.FeatureBattery && key == domain.StateBatteryLvl:
		// Matter BatPercentRemaining is 0..200 (half-percent). Normalise to 0..100.
		n, err := asInt(v)
		if err != nil {
			return nil, err
		}
		return n / 2, nil
	case feature == domain.FeaturePowerMeter:
		return v, nil

	// --- colour ---
	case feature == domain.FeatureColor && key == domain.StateColorHue:
		n, err := asInt(v)
		if err != nil {
			return nil, err
		}
		return HueToDegrees(n), nil
	case feature == domain.FeatureColor && key == domain.StateColorSat:
		n, err := asInt(v)
		if err != nil {
			return nil, err
		}
		return SaturationToPercent(n), nil
	case feature == domain.FeatureColor && key == domain.StateColorMode:
		return enumValue(v, colorModes)

	// --- environment ---
	case feature == domain.FeatureIlluminance:
		n, err := asInt(v)
		if err != nil {
			return nil, err
		}
		return LogLuxToLux(n), nil
	case feature == domain.FeaturePressure:
		n, err := asInt(v)
		if err != nil {
			return nil, err
		}
		// PressureMeasurement is kPa*10; keystone stores hPa.
		return float32(n), nil
	case feature == domain.FeatureFlow:
		n, err := asInt(v)
		if err != nil {
			return nil, err
		}
		// FlowMeasurement is m³/h * 10.
		return float32(n) / 10.0, nil
	case feature == domain.FeatureAirQuality && key == domain.StateAirQualityIndex:
		return enumValue(v, airQualityLevels)
	case feature == domain.FeatureAirQuality:
		// Concentration clusters report a float in the cluster's own unit.
		return asFloat(v)
	case (feature == domain.FeatureSmoke || feature == domain.FeatureCO) && key == domain.StateAlarm:
		state, err := enumValue(v, alarmStates)
		if err != nil {
			return nil, err
		}
		// Rules want a boolean "is it alarming"; the raw level stays available
		// through the event stream.
		return state != "normal", nil

	// --- lock / cover ---
	case feature == domain.FeatureLock && key == domain.StateLocked:
		state, err := enumValue(v, lockStates)
		if err != nil {
			return nil, err
		}
		return state == "locked", nil
	case feature == domain.FeatureCoverPosition && key == domain.StateLevel:
		n, err := asInt(v)
		if err != nil {
			return nil, err
		}
		// Matter counts how far the covering is closed; users think in "open".
		return 100 - HundredthsToPercent(n), nil

	// --- climate ---
	case feature == domain.FeatureThermostat && (key == domain.StateTargetHeat || key == domain.StateTargetCool):
		n, err := asInt(v)
		if err != nil {
			return nil, err
		}
		return CentiCelsius(n), nil
	case feature == domain.FeatureThermostat && key == domain.StateHVACMode:
		return enumValue(v, hvacModes)
	case feature == domain.FeatureThermostat && key == domain.StateHVACRunning:
		// ThermostatRunningState is a bitmap, not an enum.
		n, err := asInt(v)
		if err != nil {
			return nil, err
		}
		return runningStateLabel(n), nil
	case feature == domain.FeatureFan && key == domain.StateFanMode:
		return enumValue(v, fanModes)
	case feature == domain.FeatureFan && key == domain.StateFanPercent:
		return asInt(v)

	// --- appliances ---
	case feature == domain.FeatureRunState && key == domain.StateRunState:
		return enumValue(v, operationalStates)
	case feature == domain.FeatureRunState && key == domain.StateCountdown:
		return asInt(v)
	case feature == domain.FeatureMode && key == domain.StateMode:
		return asInt(v)
	case feature == domain.FeatureValve && key == domain.StateValveOpen:
		state, err := enumValue(v, valveStates)
		if err != nil {
			return nil, err
		}
		return state == "open", nil
	case feature == domain.FeatureValve && key == domain.StateValveLevel:
		return asInt(v)
	case feature == domain.FeatureValve && key == domain.StateValveRemaining:
		return asInt(v)
	case feature == domain.FeatureTempControl && key == domain.StateSetpoint:
		n, err := asInt(v)
		if err != nil {
			return nil, err
		}
		return CentiCelsius(n), nil

	// --- media / EV ---
	case feature == domain.FeatureMedia && key == domain.StatePlayback:
		return enumValue(v, playbackStates)
	case feature == domain.FeatureEVSE && key == domain.StateEVSEState:
		return enumValue(v, evseStates)
	case feature == domain.FeatureEVSE && key == domain.StateEVSESupply:
		return enumValue(v, evseSupplyStates)

	default:
		return v, nil
	}
}

// runningStateLabel turns the ThermostatRunningState bitmap into the single
// word a user cares about. Heating wins over cooling when both bits are set,
// which only happens on misbehaving firmware.
func runningStateLabel(bitmap int) string {
	switch {
	case bitmap&0x01 != 0:
		return "heating"
	case bitmap&0x02 != 0:
		return "cooling"
	case bitmap&0x04 != 0:
		return "fan"
	case bitmap == 0:
		return "idle"
	default:
		return "running"
	}
}

// asFloat accepts the numeric shapes json.Unmarshal can produce.
func asFloat(v any) (float32, error) {
	switch n := v.(type) {
	case float64:
		return float32(n), nil
	case float32:
		return n, nil
	case int:
		return float32(n), nil
	case int64:
		return float32(n), nil
	default:
		return 0, typeMismatch("number", v)
	}
}

// encodeAttribute is the inverse used by writeAttribute. Only writable
// attributes are handled; read-only sensor keys return an error.
func encodeAttribute(feature domain.FeatureKey, key domain.StateKey, value any) (any, error) {
	switch {
	case feature == domain.FeatureOnOff && key == domain.StateOnOff:
		b, ok := value.(bool)
		if !ok {
			return nil, typeMismatch("bool", value)
		}
		return b, nil
	case feature == domain.FeatureThermostat && (key == domain.StateTargetHeat || key == domain.StateTargetCool):
		f, err := asFloat(value)
		if err != nil {
			return nil, err
		}
		// Thermostat setpoints are int16 in 0.01 °C.
		return int(f * 100), nil
	case feature == domain.FeatureThermostat && key == domain.StateHVACMode:
		return encodeEnum(value, hvacModes)
	case feature == domain.FeatureFan && key == domain.StateFanMode:
		return encodeEnum(value, fanModes)
	case feature == domain.FeatureFan && key == domain.StateFanPercent:
		n, err := asInt(value)
		if err != nil {
			return nil, err
		}
		return clampPercent(n), nil
	case feature == domain.FeatureMode && key == domain.StateMode:
		return asInt(value)
	case feature == domain.FeatureCoverPosition && key == domain.StateLevel:
		n, err := asInt(value)
		if err != nil {
			return nil, err
		}
		return PercentToHundredths(100 - clampPercent(n)), nil

	default:
		return nil, fmt.Errorf("matter: writing feature=%s state=%s is not supported", feature, key)
	}
}

// encodeEnum maps a keystone enum word back to its Matter numeric value.
func encodeEnum(value any, table map[int]string) (any, error) {
	switch v := value.(type) {
	case string:
		for n, name := range table {
			if name == v {
				return n, nil
			}
		}
		return nil, fmt.Errorf("matter: %q is not one of %s", v, strings.Join(sortedValues(table), ", "))
	default:
		return asInt(value)
	}
}

func sortedValues(table map[int]string) []string {
	out := make([]string, 0, len(table))
	for _, v := range table {
		out = append(out, v)
	}
	sort.Strings(out)
	return out
}

// actionToInvoke maps a keystone (Feature, Action, params) triple onto the
// Matter cluster command to invoke.
func actionToInvoke(ref domain.TransportRef, endpoint int, clusters []string, feature domain.FeatureKey, action domain.ActionKey, params map[string]any) (InvokeParams, error) {
	base := InvokeParams{NodeID: string(ref), EndpointID: endpoint}
	switch feature {
	case domain.FeatureOnOff:
		base.Cluster = ClusterOnOff
		switch action {
		case domain.ActionTurnOn:
			base.Command = CmdOn
		case domain.ActionTurnOff:
			base.Command = CmdOff
		case domain.ActionToggle:
			base.Command = CmdToggle
		default:
			return base, fmt.Errorf("matter: onoff has no action %s", action)
		}
		return base, nil

	case domain.FeatureBrightness:
		if action != domain.ActionSet {
			return base, fmt.Errorf("matter: brightness has no action %s", action)
		}
		n, err := asInt(params["level"])
		if err != nil {
			return base, fmt.Errorf("matter: brightness set: params.level: %w", err)
		}
		base.Cluster = ClusterLevelControl
		// WithOnOff: setting a brightness on a lamp that is off should light it,
		// which is what every other app does and what the user means. Plain
		// MoveToLevel is accepted and does nothing visible while the lamp is off.
		base.Command = CmdMoveToLevelWithOnOff
		// The options pair is mandatory even here, where we do not need to
		// override anything: WithOnOff already lights the lamp.
		base.Args = withOptions(map[string]any{
			"level":          PercentToLevel(n),
			"transitionTime": 0,
		})
		return base, nil

	case domain.FeatureColorTemp:
		if action != domain.ActionSet {
			return base, fmt.Errorf("matter: color_temp has no action %s", action)
		}
		n, err := asInt(params["kelvin"])
		if err != nil {
			return base, fmt.Errorf("matter: color_temp set: params.kelvin: %w", err)
		}
		base.Cluster = ClusterColorControl
		base.Command = CmdMoveToColorTempMireds
		base.Args = executeIfOff(map[string]any{
			"colorTemperatureMireds": KelvinToMireds(n),
			"transitionTime":         0,
		})
		return base, nil

	case domain.FeatureColor:
		if action != domain.ActionSet {
			return base, fmt.Errorf("matter: color has no action %s", action)
		}
		base.Cluster = ClusterColorControl
		// Accept either hue/saturation or CIE xy — a UI colour wheel produces
		// the former, a colour picker calibrated against a profile the latter.
		if x, okX := params["x"]; okX {
			y, okY := params["y"]
			if !okY {
				return base, fmt.Errorf("matter: color set: params.x given without params.y")
			}
			fx, err := asFloat(x)
			if err != nil {
				return base, fmt.Errorf("matter: color set: params.x: %w", err)
			}
			fy, err := asFloat(y)
			if err != nil {
				return base, fmt.Errorf("matter: color set: params.y: %w", err)
			}
			base.Command = CmdMoveToColor
			base.Args = executeIfOff(map[string]any{
				"colorX":         CieToMatter(float64(fx)),
				"colorY":         CieToMatter(float64(fy)),
				"transitionTime": 0,
			})
			return base, nil
		}
		// Hue and saturation can be set together or one at a time; a slider that
		// only moves hue must not have to invent a saturation value.
		rawHue, hasHue := params["hue"]
		rawSat, hasSat := params["saturation"]
		switch {
		case hasHue && hasSat:
			hue, err := asInt(rawHue)
			if err != nil {
				return base, fmt.Errorf("matter: color set: params.hue: %w", err)
			}
			sat, err := asInt(rawSat)
			if err != nil {
				return base, fmt.Errorf("matter: color set: params.saturation: %w", err)
			}
			base.Command = CmdMoveToHueAndSaturation
			base.Args = executeIfOff(map[string]any{
				"hue":            DegreesToHue(hue),
				"saturation":     PercentToSaturation(sat),
				"transitionTime": 0,
			})
		case hasHue:
			hue, err := asInt(rawHue)
			if err != nil {
				return base, fmt.Errorf("matter: color set: params.hue: %w", err)
			}
			base.Command = CmdMoveToHue
			base.Args = executeIfOff(map[string]any{
				"hue":            DegreesToHue(hue),
				"direction":      0, // shortest path
				"transitionTime": 0,
			})
		case hasSat:
			sat, err := asInt(rawSat)
			if err != nil {
				return base, fmt.Errorf("matter: color set: params.saturation: %w", err)
			}
			base.Command = CmdMoveToSaturation
			base.Args = executeIfOff(map[string]any{
				"saturation":     PercentToSaturation(sat),
				"transitionTime": 0,
			})
		default:
			return base, fmt.Errorf("matter: color set needs hue/saturation or x/y")
		}
		return base, nil

	case domain.FeatureLock:
		base.Cluster = ClusterDoorLock
		switch action {
		case domain.ActionLock:
			base.Command = CmdLockDoor
		case domain.ActionUnlock:
			base.Command = CmdUnlockDoor
		default:
			return base, fmt.Errorf("matter: lock has no action %s", action)
		}
		// PIN-protected locks require the code as an argument; passing it
		// through untouched keeps that possible without modelling PINs here.
		if pin, ok := params["pin"]; ok {
			base.Args = map[string]any{"pinCode": pin}
		}
		return base, nil

	case domain.FeatureCoverPosition:
		base.Cluster = ClusterWindowCovering
		switch action {
		case domain.ActionOpen:
			base.Command = CmdUpOrOpen
		case domain.ActionClose:
			base.Command = CmdDownOrClose
		case domain.ActionStop:
			base.Command = CmdStopMotion
		case domain.ActionSet:
			n, err := asInt(params["level"])
			if err != nil {
				return base, fmt.Errorf("matter: cover set: params.level: %w", err)
			}
			base.Command = CmdGoToLiftPercentage
			// Matter counts hundredths of a percent, and 0 means fully open.
			base.Args = map[string]any{"liftPercent100thsValue": PercentToHundredths(100 - n)}
		default:
			return base, fmt.Errorf("matter: cover has no action %s", action)
		}
		return base, nil

	case domain.FeatureThermostat:
		if action != domain.ActionSet {
			return base, fmt.Errorf("matter: thermostat has no action %s", action)
		}
		return base, fmt.Errorf(
			"matter: thermostat setpoints are attributes — write %s/%s instead of invoking an action",
			domain.StateTargetHeat, domain.StateTargetCool)

	case domain.FeatureFan:
		if action != domain.ActionSet {
			return base, fmt.Errorf("matter: fan has no action %s", action)
		}
		return base, fmt.Errorf("matter: fan speed is an attribute — write %s instead", domain.StateFanPercent)

	case domain.FeatureMode:
		if action != domain.ActionSet {
			return base, fmt.Errorf("matter: mode has no action %s", action)
		}
		n, err := asInt(params["mode"])
		if err != nil {
			return base, fmt.Errorf("matter: mode set: params.mode: %w", err)
		}
		// CurrentMode is read-only on every mode cluster; the change goes
		// through ChangeToMode, which all of them share.
		base.Cluster = pickCluster(clusters,
			ClusterRvcRunMode, ClusterLaundryWasherMode, ClusterDishwasherMode,
			ClusterRefrigeratorMode, ClusterWaterHeaterMode, ClusterEvseMode, ClusterModeSelect)
		base.Command = CmdChangeToMode
		base.Args = map[string]any{"newMode": n}
		return base, nil

	case domain.FeatureValve:
		base.Cluster = ClusterValve
		switch action {
		case domain.ActionOpen:
			base.Command = CmdValveOpen
			args := map[string]any{}
			// Both arguments are optional; passing them only when asked keeps
			// simple on/off valves working.
			if d, ok := params["duration_s"]; ok {
				n, err := asInt(d)
				if err != nil {
					return base, fmt.Errorf("matter: valve open: params.duration_s: %w", err)
				}
				args["openDuration"] = n
			}
			if l, ok := params["level"]; ok {
				n, err := asInt(l)
				if err != nil {
					return base, fmt.Errorf("matter: valve open: params.level: %w", err)
				}
				args["targetLevel"] = clampPercent(n)
			}
			if len(args) > 0 {
				base.Args = args
			}
		case domain.ActionClose:
			base.Command = CmdValveClose
		case domain.ActionSet:
			n, err := asInt(params["level"])
			if err != nil {
				return base, fmt.Errorf("matter: valve set: params.level: %w", err)
			}
			// A proportional valve is opened *to* a level.
			base.Command = CmdValveOpen
			base.Args = map[string]any{"targetLevel": clampPercent(n)}
		default:
			return base, fmt.Errorf("matter: valve has no action %s", action)
		}
		return base, nil

	case domain.FeatureTempControl:
		if action != domain.ActionSet {
			return base, fmt.Errorf("matter: temp_control has no action %s", action)
		}
		f, err := asFloat(params["celsius"])
		if err != nil {
			return base, fmt.Errorf("matter: temp_control set: params.celsius: %w", err)
		}
		base.Cluster = ClusterTemperatureControl
		base.Command = CmdSetTemperature
		base.Args = map[string]any{"targetTemperature": int(f * 100)}
		return base, nil

	case domain.FeatureRunState:
		// OperationalState and its RVC twin share command names; the endpoint's
		// cluster set decides which one the adapter addresses. A vacuum exposes
		// only the RVC variant, so defaulting to the base cluster would send
		// Start to a cluster the device does not have.
		base.Cluster = pickCluster(clusters, ClusterRvcOperationalState, ClusterOperationalState)
		switch action {
		case domain.ActionStart:
			base.Command = CmdOpStart
		case domain.ActionStop:
			base.Command = CmdOpStop
		case domain.ActionPause:
			base.Command = CmdOpPause
		case domain.ActionResume:
			base.Command = CmdOpResume
		default:
			return base, fmt.Errorf("matter: run_state has no action %s", action)
		}
		return base, nil

	case domain.FeatureMedia:
		base.Cluster = ClusterMediaPlayback
		switch action {
		case domain.ActionStart:
			base.Command = CmdPlay
		case domain.ActionPause:
			base.Command = CmdPause
		case domain.ActionStop:
			base.Command = CmdStop
		case domain.ActionNext:
			base.Command = CmdNext
		case domain.ActionPrev:
			base.Command = CmdPrevious
		default:
			return base, fmt.Errorf("matter: media has no action %s", action)
		}
		return base, nil

	case domain.FeatureSmoke:
		if action != domain.ActionSelfTest {
			return base, fmt.Errorf("matter: smoke has no action %s", action)
		}
		base.Cluster = ClusterSmokeCoAlarm
		base.Command = CmdSelfTestRequest
		return base, nil

	case domain.FeatureEVSE:
		base.Cluster = ClusterEnergyEvse
		switch action {
		case domain.ActionChargeEnable:
			base.Command = CmdEnableCharging
			// The spec requires a charging window and current limits; without
			// explicit values, enable indefinitely at the circuit's own maximum.
			args := map[string]any{"minimumChargeCurrent": 0}
			if until, ok := params["until"]; ok {
				args["chargingEnabledUntil"] = until
			} else {
				args["chargingEnabledUntil"] = nil // null = no expiry
			}
			if maxCurrent, ok := params["max_current_ma"]; ok {
				n, err := asInt(maxCurrent)
				if err != nil {
					return base, fmt.Errorf("matter: evse: params.max_current_ma: %w", err)
				}
				args["maximumChargeCurrent"] = n
			}
			base.Args = args
		case domain.ActionChargeDisable:
			base.Command = CmdEvseDisable
		default:
			return base, fmt.Errorf("matter: evse has no action %s", action)
		}
		return base, nil

	case domain.FeatureCamera:
		switch action {
		case domain.ActionSnapshot:
			base.Cluster = ClusterCameraAvStream
			base.Command = CmdCaptureSnapshot
			if id, ok := params["stream_id"]; ok {
				base.Args = map[string]any{"snapshotStreamId": id}
			}
			return base, nil
		case domain.ActionMove:
			base.Cluster = ClusterCameraPTZ
			base.Command = CmdMptzSetPosition
			args := map[string]any{}
			for _, axis := range []string{"pan", "tilt", "zoom"} {
				v, ok := params[axis]
				if !ok {
					continue
				}
				n, err := asInt(v)
				if err != nil {
					return base, fmt.Errorf("matter: camera move: params.%s: %w", axis, err)
				}
				args[axis] = n
			}
			if len(args) == 0 {
				return base, fmt.Errorf("matter: camera move needs at least one of pan/tilt/zoom")
			}
			base.Args = args
			return base, nil
		default:
			return base, fmt.Errorf("matter: camera has no action %s", action)
		}

	case domain.FeatureChime:
		if action != domain.ActionRing {
			return base, fmt.Errorf("matter: chime has no action %s", action)
		}
		base.Cluster = ClusterChime
		base.Command = CmdPlayChimeSound
		return base, nil

	default:
		return base, fmt.Errorf("matter: no action mapping for feature %s", feature)
	}
}

// pickCluster returns the first candidate the endpoint actually exposes,
// falling back to the last candidate when the layout is unknown.
func pickCluster(clusters []string, candidates ...string) string {
	for _, c := range candidates {
		for _, have := range clusters {
			if have == c {
				return c
			}
		}
	}
	return candidates[len(candidates)-1]
}

// withOptions fills the OptionsMask/OptionsOverride pair that LevelControl and
// ColorControl commands declare as mandatory. Omitting them is not a lenient
// "use defaults" — the command fails validation and the device never sees it,
// which is exactly how brightness silently did nothing.
func withOptions(args map[string]any) map[string]any {
	args["optionsMask"] = 0
	args["optionsOverride"] = 0
	return args
}

// executeIfOff makes a ColorControl command apply even when the light is off.
// Without it the spec says the device ignores the command outright, so setting
// a colour on a lamp that is off would silently do nothing — the mask selects
// the ExecuteIfOff bit and the override sets it.
func executeIfOff(args map[string]any) map[string]any {
	args["optionsMask"] = 1
	args["optionsOverride"] = 1
	return args
}

// asInt accepts json.Number, float64, int, int64 uniformly. Sidecar values
// arrive as float64 after json.Unmarshal into interface{}.
func asInt(v any) (int, error) {
	switch n := v.(type) {
	case int:
		return n, nil
	case int64:
		return int(n), nil
	case float64:
		return int(n), nil
	case float32:
		return int(n), nil
	default:
		return 0, typeMismatch("number", v)
	}
}

func typeMismatch(want string, got any) error {
	return fmt.Errorf("matter: expected %s, got %T (%v)", want, got, got)
}
