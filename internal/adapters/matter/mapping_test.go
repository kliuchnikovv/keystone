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
	b, err := BindingFor(domain.FeatureOnOff, domain.StateOnOff)
	if err != nil {
		t.Fatalf("BindingFor OnOff: %v", err)
	}
	if b.Cluster != ClusterOnOff || b.Attribute != AttrOnOff {
		t.Errorf("wrong binding: %+v", b)
	}
	if _, err := BindingFor("nope", "nope"); err == nil {
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

	on, err := actionToInvoke(ref, domain.FeatureOnOff, domain.ActionTurnOn, nil)
	if err != nil || on.Cluster != ClusterOnOff || on.Command != CmdOn || on.NodeID != "node-42" {
		t.Errorf("turn_on: %+v err=%v", on, err)
	}

	bright, err := actionToInvoke(ref, domain.FeatureBrightness, domain.ActionSet, map[string]any{"level": 50})
	if err != nil {
		t.Fatalf("brightness set: %v", err)
	}
	if bright.Command != CmdMoveToLevel {
		t.Errorf("brightness command: %s", bright.Command)
	}
	if got := bright.Args["level"].(int); got != 127 {
		t.Errorf("brightness level scaled: got %d want 127", got)
	}

	if _, err := actionToInvoke(ref, domain.FeatureBrightness, domain.ActionSet, map[string]any{"level": "oops"}); err == nil {
		t.Error("expected error for non-numeric level")
	}
	if _, err := actionToInvoke(ref, domain.FeatureOnOff, domain.ActionSet, nil); err == nil {
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
	d := nodeToDiscovered(n)
	if d.Type != domain.DeviceTypeLight {
		t.Errorf("type = %s want light", d.Type)
	}
	if d.Manufacturer != "IKEA" || d.Model != "WARMBLIXT" {
		t.Errorf("labels: %+v", d)
	}
	if len(d.Features) != 3 {
		t.Errorf("features count = %d want 3", len(d.Features))
	}
	if d.TransportRef != "abc" {
		t.Errorf("transport ref = %s", d.TransportRef)
	}
}
