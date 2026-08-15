package matter

import (
	"testing"

	"github.com/kliuchnikovv/keystone/internal/domain"
)

// The same keystone capability is carried by different Matter clusters
// depending on the appliance. The endpoint's cluster set has to decide, or a
// vacuum's mode would be read from a washer's cluster.
func TestBindingResolvesByEndpointClusters(t *testing.T) {
	cases := []struct {
		name     string
		feature  domain.FeatureKey
		state    domain.StateKey
		clusters []string
		want     string
	}{
		{"vacuum mode", domain.FeatureMode, domain.StateMode,
			[]string{ClusterOnOff, ClusterRvcRunMode}, ClusterRvcRunMode},
		{"washer mode", domain.FeatureMode, domain.StateMode,
			[]string{ClusterLaundryWasherMode}, ClusterLaundryWasherMode},
		{"dishwasher mode", domain.FeatureMode, domain.StateMode,
			[]string{ClusterDishwasherMode}, ClusterDishwasherMode},
		{"generic mode falls back to ModeSelect", domain.FeatureMode, domain.StateMode,
			[]string{ClusterModeSelect}, ClusterModeSelect},
		{"specific mode cluster beats the generic one", domain.FeatureMode, domain.StateMode,
			[]string{ClusterModeSelect, ClusterRvcRunMode}, ClusterRvcRunMode},
		{"legacy plug power", domain.FeaturePowerMeter, domain.StatePowerNow,
			[]string{ClusterElectricalMeas}, ClusterElectricalMeas},
		{"matter 1.3 power", domain.FeaturePowerMeter, domain.StatePowerNow,
			[]string{ClusterElectricalPower}, ClusterElectricalPower},
		{"thermostat reports room temperature", domain.FeatureTemperature, domain.StateTemperature,
			[]string{ClusterThermostat}, ClusterThermostat},
		{"bare sensor reports room temperature", domain.FeatureTemperature, domain.StateTemperature,
			[]string{ClusterTemperatureMeas}, ClusterTemperatureMeas},
		{"vacuum run state", domain.FeatureRunState, domain.StateRunState,
			[]string{ClusterRvcOperationalState}, ClusterRvcOperationalState},
		{"appliance run state", domain.FeatureRunState, domain.StateRunState,
			[]string{ClusterOperationalState}, ClusterOperationalState},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			b, err := BindingFor(tc.feature, tc.state, tc.clusters)
			if err != nil {
				t.Fatalf("BindingFor: %v", err)
			}
			if b.Cluster != tc.want {
				t.Errorf("cluster = %s want %s", b.Cluster, tc.want)
			}
		})
	}
}

// An endpoint that has none of the candidate clusters must produce a clear
// error rather than silently addressing a cluster the device lacks.
func TestBindingRejectsUnrelatedEndpoint(t *testing.T) {
	_, err := BindingFor(domain.FeatureMode, domain.StateMode, []string{ClusterOnOff})
	if err == nil {
		t.Fatal("expected an error when no candidate cluster is present")
	}
}

// With no endpoint information the adapter must still work the way it did
// before endpoint-aware routing existed.
func TestBindingFallsBackWithoutClusterInfo(t *testing.T) {
	b, err := BindingFor(domain.FeaturePowerMeter, domain.StatePowerNow, nil)
	if err != nil {
		t.Fatalf("BindingFor: %v", err)
	}
	if b.Cluster == "" {
		t.Error("expected the first candidate, got nothing")
	}
}

// One device type per row of the coverage matrix: the clusters a real device of
// that class advertises must produce the features that make it controllable.
func TestCoverageByDeviceClass(t *testing.T) {
	cases := []struct {
		name       string
		deviceType string
		clusters   []string
		wantType   domain.DeviceType
		wantFeats  []domain.FeatureKey
	}{
		{
			"RGB light", "ExtendedColorLight",
			[]string{ClusterOnOff, ClusterLevelControl, ClusterColorControl},
			domain.DeviceTypeLight,
			[]domain.FeatureKey{domain.FeatureOnOff, domain.FeatureBrightness, domain.FeatureColorTemp, domain.FeatureColor},
		},
		{
			"metering plug", "OnOffPlugInUnit",
			[]string{ClusterOnOff, ClusterElectricalPower, ClusterElectricalEnergy},
			domain.DeviceTypePlug,
			[]domain.FeatureKey{domain.FeatureOnOff, domain.FeaturePowerMeter},
		},
		{
			"door lock", "DoorLock",
			[]string{ClusterDoorLock, ClusterPowerSource},
			domain.DeviceTypeLock,
			[]domain.FeatureKey{domain.FeatureLock, domain.FeatureBattery},
		},
		{
			"blind", "WindowCovering",
			[]string{ClusterWindowCovering},
			domain.DeviceTypeCover,
			[]domain.FeatureKey{domain.FeatureCoverPosition},
		},
		{
			"remote", "GenericSwitch",
			[]string{ClusterSwitch, ClusterPowerSource},
			domain.DeviceTypeButton,
			[]domain.FeatureKey{domain.FeatureButton, domain.FeatureBattery},
		},
		{
			"thermostat", "Thermostat",
			[]string{ClusterThermostat},
			domain.DeviceTypeThermostat,
			[]domain.FeatureKey{domain.FeatureThermostat, domain.FeatureTemperature},
		},
		{
			"air purifier", "AirPurifier",
			[]string{ClusterFanControl, ClusterAirQuality, ClusterPM25, ClusterCO2},
			domain.DeviceTypeAirPurifier,
			[]domain.FeatureKey{domain.FeatureFan, domain.FeatureAirQuality},
		},
		{
			"smoke alarm", "SmokeCoAlarm",
			[]string{ClusterSmokeCoAlarm, ClusterPowerSource},
			domain.DeviceTypeAlarm,
			[]domain.FeatureKey{domain.FeatureSmoke, domain.FeatureCO, domain.FeatureBattery},
		},
		{
			"light sensor", "LightSensor",
			[]string{ClusterIlluminance},
			domain.DeviceTypeSensor,
			[]domain.FeatureKey{domain.FeatureIlluminance},
		},
		{
			"robot vacuum", "RoboticVacuumCleaner",
			[]string{ClusterRvcRunMode, ClusterRvcOperationalState},
			domain.DeviceTypeVacuum,
			[]domain.FeatureKey{domain.FeatureMode, domain.FeatureRunState},
		},
		{
			"washing machine", "LaundryWasher",
			[]string{ClusterLaundryWasherMode, ClusterOperationalState, ClusterOnOff},
			domain.DeviceTypeAppliance,
			[]domain.FeatureKey{domain.FeatureMode, domain.FeatureRunState, domain.FeatureOnOff},
		},
		{
			"speaker", "Speaker",
			[]string{ClusterMediaPlayback, ClusterLevelControl, ClusterOnOff},
			domain.DeviceTypeMediaPlayer,
			[]domain.FeatureKey{domain.FeatureMedia, domain.FeatureBrightness, domain.FeatureOnOff},
		},
		{
			"EV charger", "EnergyEvse",
			[]string{ClusterEnergyEvse, ClusterElectricalPower},
			domain.DeviceTypeEVCharger,
			[]domain.FeatureKey{domain.FeatureEVSE, domain.FeaturePowerMeter},
		},
		{
			"water valve", "WaterValve",
			[]string{ClusterValve},
			domain.DeviceTypeValve,
			[]domain.FeatureKey{domain.FeatureValve},
		},
		{
			"oven with a temperature dial", "Oven",
			[]string{ClusterTemperatureControl, ClusterOperationalState},
			domain.DeviceTypeAppliance,
			[]domain.FeatureKey{domain.FeatureTempControl, domain.FeatureRunState},
		},
		{
			"generic multi-mode device", "AirPurifier",
			[]string{ClusterModeSelect, ClusterFanControl},
			domain.DeviceTypeAirPurifier,
			[]domain.FeatureKey{domain.FeatureMode, domain.FeatureFan},
		},
		{
			"camera", "Camera",
			[]string{ClusterCameraAvStream, ClusterCameraPTZ},
			domain.DeviceTypeCamera,
			[]domain.FeatureKey{domain.FeatureCamera},
		},
		{
			"video doorbell", "VideoDoorbell",
			[]string{ClusterCameraAvStream, ClusterChime},
			domain.DeviceTypeDoorbell,
			[]domain.FeatureKey{domain.FeatureCamera, domain.FeatureChime},
		},
	}

	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			n := Node{
				NodeID: "n1",
				Endpoints: []Endpoint{
					{EndpointID: 0, DeviceType: "RootNode"},
					{EndpointID: 1, DeviceType: tc.deviceType, Clusters: tc.clusters},
				},
			}
			d, _ := nodeToDiscovered(n)
			if d.Type != tc.wantType {
				t.Errorf("type = %s want %s", d.Type, tc.wantType)
			}
			have := map[domain.FeatureKey]bool{}
			for _, f := range d.Features {
				have[f.Key] = true
			}
			for _, want := range tc.wantFeats {
				if !have[want] {
					t.Errorf("missing feature %s (got %v)", want, d.Features)
				}
			}
		})
	}
}

// Air quality is assembled from several concentration clusters. Merging them
// into one feature is the whole point; dropping all but the first would leave a
// sensor reporting only its index.
func TestAirQualityMergesConcentrationClusters(t *testing.T) {
	n := Node{
		NodeID: "aq",
		Endpoints: []Endpoint{{EndpointID: 1, DeviceType: "AirQualitySensor", Clusters: []string{
			ClusterAirQuality, ClusterPM25, ClusterPM10, ClusterCO2, ClusterTVOC, ClusterFormaldehyde,
		}}},
	}
	d, _ := nodeToDiscovered(n)
	var aq *domain.Feature
	for i := range d.Features {
		if d.Features[i].Key == domain.FeatureAirQuality {
			aq = &d.Features[i]
		}
	}
	if aq == nil {
		t.Fatal("no air_quality feature")
	}
	for _, want := range []domain.StateKey{
		domain.StateAirQualityIndex, domain.StatePM25, domain.StatePM10,
		domain.StateCO2, domain.StateTVOC, domain.StateFormaldehyde,
	} {
		found := false
		for _, s := range aq.States {
			if s == want {
				found = true
			}
		}
		if !found {
			t.Errorf("missing state %s (got %v)", want, aq.States)
		}
	}
}

// Control, not just classification: every controllable feature must produce a
// real cluster command.
func TestCommandsForControllableFeatures(t *testing.T) {
	cases := []struct {
		feature  domain.FeatureKey
		action   domain.ActionKey
		params   map[string]any
		clusters []string
		cluster  string
		command  string
	}{
		{domain.FeatureLock, domain.ActionLock, nil, nil, ClusterDoorLock, CmdLockDoor},
		{domain.FeatureLock, domain.ActionUnlock, nil, nil, ClusterDoorLock, CmdUnlockDoor},
		{domain.FeatureCoverPosition, domain.ActionOpen, nil, nil, ClusterWindowCovering, CmdUpOrOpen},
		{domain.FeatureCoverPosition, domain.ActionClose, nil, nil, ClusterWindowCovering, CmdDownOrClose},
		{domain.FeatureCoverPosition, domain.ActionStop, nil, nil, ClusterWindowCovering, CmdStopMotion},
		{domain.FeatureCoverPosition, domain.ActionSet, map[string]any{"level": 40}, nil, ClusterWindowCovering, CmdGoToLiftPercentage},
		{domain.FeatureColor, domain.ActionSet, map[string]any{"hue": 120, "saturation": 100}, nil, ClusterColorControl, CmdMoveToHueAndSaturation},
		{domain.FeatureColor, domain.ActionSet, map[string]any{"x": 0.3, "y": 0.4}, nil, ClusterColorControl, CmdMoveToColor},
		{domain.FeatureMedia, domain.ActionStart, nil, nil, ClusterMediaPlayback, CmdPlay},
		{domain.FeatureMedia, domain.ActionNext, nil, nil, ClusterMediaPlayback, CmdNext},
		{domain.FeatureRunState, domain.ActionStart, nil, []string{ClusterRvcOperationalState}, ClusterRvcOperationalState, CmdOpStart},
		{domain.FeatureRunState, domain.ActionPause, nil, []string{ClusterOperationalState}, ClusterOperationalState, CmdOpPause},
		{domain.FeatureSmoke, domain.ActionSelfTest, nil, nil, ClusterSmokeCoAlarm, CmdSelfTestRequest},
		{domain.FeatureEVSE, domain.ActionChargeEnable, nil, nil, ClusterEnergyEvse, CmdEnableCharging},
		{domain.FeatureEVSE, domain.ActionChargeDisable, nil, nil, ClusterEnergyEvse, CmdEvseDisable},
		{domain.FeatureCamera, domain.ActionSnapshot, nil, nil, ClusterCameraAvStream, CmdCaptureSnapshot},
		{domain.FeatureCamera, domain.ActionMove, map[string]any{"pan": 10}, nil, ClusterCameraPTZ, CmdMptzSetPosition},
		{domain.FeatureChime, domain.ActionRing, nil, nil, ClusterChime, CmdPlayChimeSound},
		{domain.FeatureValve, domain.ActionOpen, nil, nil, ClusterValve, CmdValveOpen},
		{domain.FeatureValve, domain.ActionClose, nil, nil, ClusterValve, CmdValveClose},
		{domain.FeatureValve, domain.ActionSet, map[string]any{"level": 50}, nil, ClusterValve, CmdValveOpen},
		{domain.FeatureTempControl, domain.ActionSet, map[string]any{"celsius": 180.0}, nil, ClusterTemperatureControl, CmdSetTemperature},
		{domain.FeatureMode, domain.ActionSet, map[string]any{"mode": 2}, []string{ClusterRvcRunMode}, ClusterRvcRunMode, CmdChangeToMode},
		{domain.FeatureMode, domain.ActionSet, map[string]any{"mode": 1}, []string{ClusterModeSelect}, ClusterModeSelect, CmdChangeToMode},
	}

	for _, tc := range cases {
		t.Run(string(tc.feature)+"/"+string(tc.action), func(t *testing.T) {
			inv, err := actionToInvoke("n1", 1, tc.clusters, tc.feature, tc.action, tc.params)
			if err != nil {
				t.Fatalf("actionToInvoke: %v", err)
			}
			if inv.Cluster != tc.cluster || inv.Command != tc.command {
				t.Errorf("got %s.%s want %s.%s", inv.Cluster, inv.Command, tc.cluster, tc.command)
			}
		})
	}
}

// A cover at 40% open is 60% closed on the wire, and the round trip has to
// agree with itself — a blind that jumps when you read back what you set is
// worse than one that does not move at all.
func TestCoverPositionRoundTrip(t *testing.T) {
	encoded, err := encodeAttribute(domain.FeatureCoverPosition, domain.StateLevel, 40)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	if encoded.(int) != 6000 {
		t.Errorf("40%% open encoded as %v, want 6000 hundredths closed", encoded)
	}
	decoded, err := decodeAttributeAny(domain.FeatureCoverPosition, domain.StateLevel, float64(6000))
	if err != nil {
		t.Fatalf("decode: %v", err)
	}
	if decoded.(int) != 40 {
		t.Errorf("round trip gave %v, want 40", decoded)
	}
}

func TestEnumDecoding(t *testing.T) {
	cases := []struct {
		feature domain.FeatureKey
		key     domain.StateKey
		raw     any
		want    any
	}{
		{domain.FeatureLock, domain.StateLocked, float64(1), true},
		{domain.FeatureLock, domain.StateLocked, float64(2), false},
		{domain.FeatureLock, domain.StateLocked, "Locked", true},
		{domain.FeatureThermostat, domain.StateHVACMode, float64(4), "heat"},
		{domain.FeatureThermostat, domain.StateHVACRunning, float64(1), "heating"},
		{domain.FeatureThermostat, domain.StateHVACRunning, float64(0), "idle"},
		{domain.FeatureFan, domain.StateFanMode, float64(3), "high"},
		{domain.FeatureMedia, domain.StatePlayback, float64(0), "playing"},
		{domain.FeatureRunState, domain.StateRunState, float64(1), "running"},
		{domain.FeatureRunState, domain.StateRunState, float64(0x41), "charging"},
		{domain.FeatureEVSE, domain.StateEVSEState, float64(3), "plugged_in_charging"},
		{domain.FeatureAirQuality, domain.StateAirQualityIndex, float64(2), "fair"},
		{domain.FeatureSmoke, domain.StateAlarm, float64(0), false},
		{domain.FeatureSmoke, domain.StateAlarm, float64(2), true},
	}
	for _, tc := range cases {
		got, err := decodeAttributeAny(tc.feature, tc.key, tc.raw)
		if err != nil {
			t.Errorf("%s/%s: %v", tc.feature, tc.key, err)
			continue
		}
		if got != tc.want {
			t.Errorf("%s/%s raw=%v -> %v want %v", tc.feature, tc.key, tc.raw, got, tc.want)
		}
	}
}

// Illuminance is stored as 10000*log10(lux)+1; a linear read would report
// 40000 lux as "40000" on a scale that tops out around 65535.
func TestIlluminanceDecoding(t *testing.T) {
	// 1000 lux -> 10000*log10(1000)+1 = 30001
	got, err := decodeAttributeAny(domain.FeatureIlluminance, domain.StateIlluminance, float64(30001))
	if err != nil {
		t.Fatalf("decode: %v", err)
	}
	lux := got.(float32)
	if lux < 995 || lux > 1005 {
		t.Errorf("raw 30001 -> %v lux, want ~1000", lux)
	}
	if zero, _ := decodeAttributeAny(domain.FeatureIlluminance, domain.StateIlluminance, float64(0)); zero.(float32) != 0 {
		t.Errorf("raw 0 means unknown, got %v", zero)
	}
}

func TestThermostatSetpointEncoding(t *testing.T) {
	encoded, err := encodeAttribute(domain.FeatureThermostat, domain.StateTargetHeat, 21.5)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	if encoded.(int) != 2150 {
		t.Errorf("21.5C encoded as %v, want 2150", encoded)
	}
	mode, err := encodeAttribute(domain.FeatureThermostat, domain.StateHVACMode, "heat")
	if err != nil {
		t.Fatalf("encode mode: %v", err)
	}
	if mode.(int) != 4 {
		t.Errorf("heat encoded as %v, want 4", mode)
	}
	if _, err := encodeAttribute(domain.FeatureThermostat, domain.StateHVACMode, "toasty"); err == nil {
		t.Error("expected an error for an unknown mode")
	}
}

// A button exists only through its events: CurrentPosition says a rocker is
// held, but single/double/long press are events, so a remote is undetectable
// without this path.
func TestButtonAndAlarmEvents(t *testing.T) {
	cases := []struct {
		cluster string
		event   string
		feature domain.FeatureKey
		key     domain.EventKey
	}{
		{ClusterSwitch, "InitialPress", domain.FeatureButton, domain.EventButtonPressed},
		{ClusterSwitch, "LongPress", domain.FeatureButton, domain.EventButtonLongPress},
		{ClusterSwitch, "MultiPressComplete", domain.FeatureButton, domain.EventButtonMultiPress},
		{ClusterSmokeCoAlarm, "SmokeAlarm", domain.FeatureSmoke, domain.EventSmokeAlarm},
		{ClusterSmokeCoAlarm, "CoAlarm", domain.FeatureCO, domain.EventCOAlarm},
		{ClusterOperationalState, "OperationCompletion", domain.FeatureRunState, domain.EventCycleComplete},
		{ClusterEnergyEvse, "EvConnected", domain.FeatureEVSE, domain.EventEVConnected},
	}
	for _, tc := range cases {
		feature, key, ok := EventForCluster(tc.cluster, tc.event)
		if !ok {
			t.Errorf("%s.%s not mapped", tc.cluster, tc.event)
			continue
		}
		if feature != tc.feature || key != tc.key {
			t.Errorf("%s.%s -> %s/%s want %s/%s", tc.cluster, tc.event, feature, key, tc.feature, tc.key)
		}
	}
	if _, _, ok := EventForCluster(ClusterSwitch, "MultiPressOngoing"); ok {
		t.Error("intermediate events should not reach rules")
	}
}
