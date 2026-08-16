package dirigera

import (
	"testing"

	"github.com/kliuchnikovv/keystone/internal/domain"
)

func TestToDiscovered_LightWithBrightness(t *testing.T) {
	d := Device{
		ID:         "d1",
		Type:       "light",
		DeviceType: "light",
		Attributes: map[string]any{
			"isOn":       true,
			"lightLevel": 80.0,
			"customName": "Kitchen",
		},
	}
	d.Capabilities.CanReceive = []string{"isOn", "lightLevel"}

	ref, dt, name, features := ToDiscovered(d)
	if ref != "d1" || dt != "light" || name != "Kitchen" {
		t.Fatalf("unexpected: ref=%s dt=%s name=%s", ref, dt, name)
	}
	if len(features) != 2 {
		t.Fatalf("want 2 features (on_off + brightness), got %d", len(features))
	}
	if features[0].Key != domain.FeatureOnOff || features[1].Key != domain.FeatureBrightness {
		t.Fatalf("wrong feature keys: %+v", features)
	}
}

func TestToDiscovered_PlugOnly(t *testing.T) {
	d := Device{ID: "d2", Type: "outlet", DeviceType: "outlet", Attributes: map[string]any{"isOn": false}}
	_, dt, _, features := ToDiscovered(d)
	if dt != "plug" {
		t.Fatalf("dt = %s", dt)
	}
	if len(features) != 1 || features[0].Key != domain.FeatureOnOff {
		t.Fatalf("expected only on_off, got %+v", features)
	}
}
