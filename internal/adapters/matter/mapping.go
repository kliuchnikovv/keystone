package matter

import (
	"encoding/json"
	"fmt"
	"strconv"

	"github.com/kliuchnikovv/keystone/internal/domain"
	"github.com/kliuchnikovv/keystone/internal/ports"
)

// Matter cluster identifiers. We keep them as strings because that is what
// matter.js speaks over the wire (canonical PascalCase names from the CSA
// spec). Numeric IDs are documented for grep-ability.
const (
	ClusterOnOff            = "OnOff"                       // 0x0006
	ClusterLevelControl     = "LevelControl"                // 0x0008
	ClusterColorControl     = "ColorControl"                // 0x0300
	ClusterTemperatureMeas  = "TemperatureMeasurement"      // 0x0402
	ClusterRelativeHumidity = "RelativeHumidityMeasurement" // 0x0405
	ClusterOccupancySensing = "OccupancySensing"            // 0x0406
	ClusterBooleanState     = "BooleanState"                // 0x0045
	ClusterElectricalMeas   = "ElectricalMeasurement"       // 0x0B04
	ClusterPowerSource      = "PowerSource"                 // 0x002F
)

// Matter attribute / command names we care about in MVP.
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

	CmdOn                    = "On"
	CmdOff                   = "Off"
	CmdToggle                = "Toggle"
	CmdMoveToLevel           = "MoveToLevel"
	CmdMoveToColorTempMireds = "MoveToColorTemperature"
)

// FeatureBinding tells the adapter how to translate one keystone (Feature,
// StateKey) pair into the (cluster, attribute) Matter address.
type FeatureBinding struct {
	Cluster   string
	Attribute string
}

// featureBindings is the read/write routing table. If a (feature, key) pair
// is absent, the adapter refuses the operation rather than guessing.
var featureBindings = map[domain.FeatureKey]map[domain.StateKey]FeatureBinding{
	domain.FeatureOnOff: {
		domain.StateOnOff: {Cluster: ClusterOnOff, Attribute: AttrOnOff},
	},
	domain.FeatureBrightness: {
		domain.StateLevel: {Cluster: ClusterLevelControl, Attribute: AttrCurrentLevel},
	},
	domain.FeatureColorTemp: {
		domain.StateColorTempK: {Cluster: ClusterColorControl, Attribute: AttrColorTemperatureMireds},
	},
	domain.FeatureTemperature: {
		domain.StateTemperature: {Cluster: ClusterTemperatureMeas, Attribute: AttrMeasuredValue},
	},
	domain.FeatureHumidity: {
		domain.StateHumidity: {Cluster: ClusterRelativeHumidity, Attribute: AttrMeasuredValue},
	},
	domain.FeatureMotion: {
		domain.StateOccupied: {Cluster: ClusterOccupancySensing, Attribute: AttrOccupancy},
	},
	domain.FeatureContact: {
		domain.StateContactOpen: {Cluster: ClusterBooleanState, Attribute: AttrStateValue},
	},
	domain.FeaturePowerMeter: {
		domain.StatePowerNow:    {Cluster: ClusterElectricalMeas, Attribute: AttrActivePower},
		domain.StateEnergyTotal: {Cluster: ClusterElectricalMeas, Attribute: AttrTotalActiveEnergy},
	},
	domain.FeatureBattery: {
		domain.StateBatteryLvl: {Cluster: ClusterPowerSource, Attribute: AttrBatPercentRemaining},
	},
}

// BindingFor returns the (cluster, attribute) address for a (feature, state)
// pair, or an error if the pair is not supported.
func BindingFor(feature domain.FeatureKey, key domain.StateKey) (FeatureBinding, error) {
	if fmap, ok := featureBindings[feature]; ok {
		if b, ok := fmap[key]; ok {
			return b, nil
		}
	}
	return FeatureBinding{}, fmt.Errorf("matter: no binding for feature=%s state=%s", feature, key)
}

// FeatureForCluster is the reverse map used when translating server events
// back into (feature, state) so the ingress loop can publish them on the
// internal bus. Multiple attributes on the same cluster map to different
// features (e.g. ElectricalMeasurement.ActivePower vs .TotalActiveEnergy).
func FeatureForCluster(cluster, attribute string) (domain.FeatureKey, domain.StateKey, bool) {
	for feature, states := range featureBindings {
		for state, b := range states {
			if b.Cluster == cluster && b.Attribute == attribute {
				return feature, state, true
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
	"GenericSwitch":     domain.DeviceTypeSwitch,

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
	"SmokeCoAlarm":        domain.DeviceTypeSensor,
	"WaterLeakDetector":   domain.DeviceTypeSensor,
	"WaterFreezeDetector": domain.DeviceTypeSensor,
	"OnOffSensor":         domain.DeviceTypeSensor,

	"Thermostat":     domain.DeviceTypeThermostat,
	"WindowCovering": domain.DeviceTypeCover,
	"DoorLock":       domain.DeviceTypeLock,

	"Speaker":            domain.DeviceTypeMediaPlayer,
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
	seen := map[domain.FeatureKey]bool{}
	routes := featureRoutes{}
	var out []domain.Feature
	for _, ep := range n.Endpoints {
		if ep.EndpointID == 0 {
			continue
		}
		for _, cluster := range ep.Clusters {
			for _, f := range featuresForCluster(cluster) {
				if seen[f.Key] {
					continue
				}
				seen[f.Key] = true
				routes[f.Key] = ep.EndpointID
				out = append(out, f)
			}
		}
	}
	return out, routes
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
		return []domain.Feature{{
			Key:     domain.FeatureColorTemp,
			States:  []domain.StateKey{domain.StateColorTempK},
			Actions: []domain.ActionKey{domain.ActionSet},
		}}
	case ClusterTemperatureMeas:
		return []domain.Feature{{Key: domain.FeatureTemperature, States: []domain.StateKey{domain.StateTemperature}}}
	case ClusterRelativeHumidity:
		return []domain.Feature{{Key: domain.FeatureHumidity, States: []domain.StateKey{domain.StateHumidity}}}
	case ClusterOccupancySensing:
		return []domain.Feature{{Key: domain.FeatureMotion, States: []domain.StateKey{domain.StateOccupied}}}
	case ClusterBooleanState:
		return []domain.Feature{{Key: domain.FeatureContact, States: []domain.StateKey{domain.StateContactOpen}}}
	case ClusterElectricalMeas:
		return []domain.Feature{{Key: domain.FeaturePowerMeter, States: []domain.StateKey{domain.StatePowerNow, domain.StateEnergyTotal}}}
	case ClusterPowerSource:
		return []domain.Feature{{Key: domain.FeatureBattery, States: []domain.StateKey{domain.StateBatteryLvl}}}
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
	default:
		return v, nil
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
	case feature == domain.FeatureBrightness && key == domain.StateLevel:
		n, err := asInt(value)
		if err != nil {
			return nil, err
		}
		return PercentToLevel(n), nil
	case feature == domain.FeatureColorTemp && key == domain.StateColorTempK:
		n, err := asInt(value)
		if err != nil {
			return nil, err
		}
		return KelvinToMireds(n), nil
	default:
		return nil, fmt.Errorf("matter: writing feature=%s state=%s is not supported", feature, key)
	}
}

// actionToInvoke maps a keystone (Feature, Action, params) triple onto the
// Matter cluster command to invoke.
func actionToInvoke(ref domain.TransportRef, endpoint int, feature domain.FeatureKey, action domain.ActionKey, params map[string]any) (InvokeParams, error) {
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
		base.Command = CmdMoveToLevel
		base.Args = map[string]any{"level": PercentToLevel(n), "transitionTime": 0}
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
		base.Args = map[string]any{"colorTemperatureMireds": KelvinToMireds(n), "transitionTime": 0}
		return base, nil
	default:
		return base, fmt.Errorf("matter: no action mapping for feature %s", feature)
	}
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
