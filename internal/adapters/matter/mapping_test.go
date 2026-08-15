package matter

import (
	"testing"

	"github.com/kliuchnikovv/keystone/internal/domain"
)

func TestLevelPercentRoundTrip(t *testing.T) {
	for _, p := range []int{0, 25, 50, 75, 100} {
		lvl := PercentToLevel(p)
		got := LevelToPercent(lvl)
		if diff := got - p; diff < -1 || diff > 1 {
			t.Errorf("percent %d -> level %d -> percent %d (want ±1)", p, lvl, got)
		}
	}
}

func TestMiredsKelvinRoundTrip(t *testing.T) {
	for _, k := range []int{2200, 2700, 4000, 6500} {
		m := KelvinToMireds(k)
		got := MiredsToKelvin(m)
		if diff := got - k; diff < -15 || diff > 15 {
			t.Errorf("kelvin %d -> mireds %d -> kelvin %d (want ±15)", k, m, got)
		}
	}
}

func TestBindingFor(t *testing.T) {
	b, err := BindingFor(domain.FeatureOnOff, domain.StateOnOff, nil)
	if err != nil {
		t.Fatalf("BindingFor OnOff: %v", err)
	}
	if b.Cluster != ClusterOnOff || b.Attribute != AttrOnOff {
		t.Errorf("wrong binding: %+v", b)
	}
	if _, err := BindingFor("nope", "nope", nil); err == nil {
		t.Error("expected error for unknown pair")
	}
}

func TestFeatureForClusterReverse(t *testing.T) {
	f, s, ok := FeatureForCluster(ClusterLevelControl, AttrCurrentLevel)
	if !ok || f != domain.FeatureBrightness || s != domain.StateLevel {
		t.Errorf("reverse lookup failed: f=%s s=%s ok=%v", f, s, ok)
	}
	if _, _, ok := FeatureForCluster("Fake", "None"); ok {
		t.Error("expected reverse lookup to miss")
	}
}

func TestActionToInvoke(t *testing.T) {
	ref := domain.TransportRef("node-42")

	on, err := actionToInvoke(ref, 1, nil, domain.FeatureOnOff, domain.ActionTurnOn, nil)
	if err != nil || on.Cluster != ClusterOnOff || on.Command != CmdOn || on.NodeID != "node-42" {
		t.Errorf("turn_on: %+v err=%v", on, err)
	}

	bright, err := actionToInvoke(ref, 1, nil, domain.FeatureBrightness, domain.ActionSet, map[string]any{"level": 50})
	if err != nil {
		t.Fatalf("brightness set: %v", err)
	}
	if bright.Command != CmdMoveToLevel {
		t.Errorf("brightness command: %s", bright.Command)
	}
	if got := bright.Args["level"].(int); got != 127 {
		t.Errorf("brightness level scaled: got %d want 127", got)
	}

	if _, err := actionToInvoke(ref, 1, nil, domain.FeatureBrightness, domain.ActionSet, map[string]any{"level": "oops"}); err == nil {
		t.Error("expected error for non-numeric level")
	}
	if _, err := actionToInvoke(ref, 1, nil, domain.FeatureOnOff, domain.ActionSet, nil); err == nil {
		t.Error("expected error for onoff.set")
	}
}

func TestDecodeAttributeAny(t *testing.T) {
	// Brightness comes back as Matter level 0..254; keystone stores 0..100.
	got, err := decodeAttributeAny(domain.FeatureBrightness, domain.StateLevel, float64(127))
	if err != nil {
		t.Fatalf("brightness decode: %v", err)
	}
	if p := got.(int); p != 50 {
		t.Errorf("level 127 -> percent %d want 50", p)
	}

	// Color temperature comes back as mireds (~370 mireds ≈ 2700K).
	got, err = decodeAttributeAny(domain.FeatureColorTemp, domain.StateColorTempK, float64(370))
	if err != nil {
		t.Fatalf("color_temp decode: %v", err)
	}
	if k := got.(int); k < 2695 || k > 2710 {
		t.Errorf("mireds 370 -> kelvin %d want ~2700", k)
	}

	// Occupancy bitmap: low bit set means occupied.
	got, err = decodeAttributeAny(domain.FeatureMotion, domain.StateOccupied, float64(1))
	if err != nil {
		t.Fatalf("motion decode: %v", err)
	}
	if !got.(bool) {
		t.Error("occupancy 1 -> false, want true")
	}
}

func TestNodeToDiscoveredLight(t *testing.T) {
	n := Node{
		NodeID:      "abc",
		VendorName:  "IKEA",
		ProductName: "WARMBLIXT",
		Online:      true,
		Endpoints: []Endpoint{
			{EndpointID: 1, Clusters: []string{ClusterOnOff, ClusterLevelControl, ClusterColorControl}},
		},
	}
	d, _ := nodeToDiscovered(n)
	if d.Type != domain.DeviceTypeLight {
		t.Errorf("type = %s want light", d.Type)
	}
	if d.Manufacturer != "IKEA" || d.Model != "WARMBLIXT" {
		t.Errorf("labels: %+v", d)
	}
	// ColorControl yields two features: colour temperature and full colour are
	// separate controls, and many lamps support only the former.
	want := map[domain.FeatureKey]bool{
		domain.FeatureOnOff: true, domain.FeatureBrightness: true,
		domain.FeatureColorTemp: true, domain.FeatureColor: true,
	}
	if len(d.Features) != len(want) {
		t.Errorf("features = %d want %d: %+v", len(d.Features), len(want), d.Features)
	}
	for _, f := range d.Features {
		if !want[f.Key] {
			t.Errorf("unexpected feature %s", f.Key)
		}
		delete(want, f.Key)
	}
	for k := range want {
		t.Errorf("missing feature %s", k)
	}
	if d.TransportRef != "abc" {
		t.Errorf("transport ref = %s", d.TransportRef)
	}
}

// A plug and a light expose the same clusters, so the declared Matter device
// type is the only thing that tells them apart. Before DeviceTypeList was
// read, everything with OnOff+Level became a "light".
func TestDeviceTypeFromDeclaredType(t *testing.T) {
	cases := []struct {
		name       string
		deviceType string
		clusters   []string
		want       domain.DeviceType
	}{
		{"dimmable plug is a plug", "DimmablePlugInUnit",
			[]string{ClusterOnOff, ClusterLevelControl}, domain.DeviceTypePlug},
		{"dimmable light is a light", "DimmableLight",
			[]string{ClusterOnOff, ClusterLevelControl}, domain.DeviceTypeLight},
		{"thermostat is not a temperature sensor", "Thermostat",
			[]string{ClusterTemperatureMeas}, domain.DeviceTypeThermostat},
		{"door lock", "DoorLock", []string{ClusterOnOff}, domain.DeviceTypeLock},
		{"window covering", "WindowCovering", []string{ClusterOnOff}, domain.DeviceTypeCover},
		{"unknown type falls back to features", "SomeFutureGadget",
			[]string{ClusterOnOff, ClusterLevelControl}, domain.DeviceTypeLight},
		{"no declared type falls back to features", "",
			[]string{ClusterOccupancySensing}, domain.DeviceTypeMotion},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			n := Node{
				NodeID:    "n1",
				Endpoints: []Endpoint{{EndpointID: 1, DeviceType: tc.deviceType, Clusters: tc.clusters}},
			}
			d, _ := nodeToDiscovered(n)
			if d.Type != tc.want {
				t.Errorf("type = %s want %s", d.Type, tc.want)
			}
		})
	}
}

// Endpoint 0 carries the node's administrative clusters and a RootNode device
// type. Neither may leak into the user-visible classification.
func TestRootEndpointIgnored(t *testing.T) {
	n := Node{
		NodeID: "n1",
		Endpoints: []Endpoint{
			{EndpointID: 0, DeviceType: "RootNode", Clusters: []string{ClusterPowerSource}},
			{EndpointID: 1, DeviceType: "OnOffPlugInUnit", Clusters: []string{ClusterOnOff}},
		},
	}
	d, routes := nodeToDiscovered(n)
	if d.Type != domain.DeviceTypePlug {
		t.Errorf("type = %s want plug", d.Type)
	}
	if _, ok := routes[domain.FeatureBattery]; ok {
		t.Error("PowerSource on endpoint 0 must not become a battery feature")
	}
	if got := routes[domain.FeatureOnOff]; got != 1 {
		t.Errorf("onoff endpoint = %d want 1", got)
	}
}

// A two-socket plug puts an independent OnOff on each endpoint, and a light
// strip splits colour across segments. Every feature must carry the endpoint it
// was actually found on, not the hardcoded 1.
func TestFeatureRoutesAcrossEndpoints(t *testing.T) {
	n := Node{
		NodeID: "strip",
		Endpoints: []Endpoint{
			{EndpointID: 0, DeviceType: "RootNode"},
			{EndpointID: 3, DeviceType: "ExtendedColorLight", Clusters: []string{ClusterOnOff, ClusterLevelControl}},
			{EndpointID: 4, Clusters: []string{ClusterColorControl, ClusterOnOff}},
			{EndpointID: 5, Clusters: []string{ClusterTemperatureMeas}},
		},
	}
	d, routes := nodeToDiscovered(n)

	if got := routes[domain.FeatureOnOff]; got != 3 {
		t.Errorf("onoff endpoint = %d want 3 (lowest functional endpoint wins)", got)
	}
	if got := routes[domain.FeatureColorTemp]; got != 4 {
		t.Errorf("color_temp endpoint = %d want 4", got)
	}
	if got := routes[domain.FeatureTemperature]; got != 5 {
		t.Errorf("temperature endpoint = %d want 5", got)
	}
	if got := d.Metadata["matter.endpoint.color_temp"]; got != "4" {
		t.Errorf("metadata color_temp endpoint = %q want \"4\"", got)
	}

	// The routed endpoint must reach the invoke params, not defaultEndpoint.
	inv, err := actionToInvoke(d.TransportRef, routes[domain.FeatureColorTemp], nil,
		domain.FeatureColorTemp, domain.ActionSet, map[string]any{"kelvin": 2700})
	if err != nil {
		t.Fatalf("color_temp invoke: %v", err)
	}
	if inv.EndpointID != 4 {
		t.Errorf("invoke endpoint = %d want 4", inv.EndpointID)
	}
}

func TestDisplayNamePrefersNodeLabel(t *testing.T) {
	n := Node{NodeID: "n1", VendorName: "IKEA", ProductName: "WARMBLIXT", NodeLabel: "Kitchen strip"}
	if got := displayNameFor(n); got != "Kitchen strip" {
		t.Errorf("name = %q want the user's own label", got)
	}
	n.NodeLabel = ""
	if got := displayNameFor(n); got != "IKEA WARMBLIXT" {
		t.Errorf("name = %q want vendor + product", got)
	}
}
