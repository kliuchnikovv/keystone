package dirigera

import (
	"github.com/kliuchnikovv/keystone/internal/domain"
)

// ToDiscovered turns one hub Device into a keystone-side discovered
// device. The mapping is intentionally small: on/off and brightness are
// the two features that cover 90% of DIRIGERA lamps and plugs. Extra
// capabilities (colour, colour-temp, blinds) land in follow-up work.
func ToDiscovered(d Device) (domain.TransportRef, domain.DeviceType, string, []domain.Feature) {
	name := ""
	if raw, ok := d.Attributes["customName"].(string); ok && raw != "" {
		name = raw
	}
	if name == "" {
		if model, ok := d.Attributes["model"].(string); ok && model != "" {
			name = model
		}
	}
	if name == "" {
		name = d.DeviceType
	}

	deviceType := deviceTypeFor(d)
	features := featuresFor(d)
	return domain.TransportRef(d.ID), deviceType, name, features
}

func deviceTypeFor(d Device) domain.DeviceType {
	switch d.DeviceType {
	case "light":
		return "light"
	case "outlet":
		return "plug"
	default:
		return domain.DeviceType(d.DeviceType)
	}
}

func featuresFor(d Device) []domain.Feature {
	features := []domain.Feature{}
	if _, ok := d.Attributes["isOn"]; ok {
		features = append(features, domain.Feature{
			Key:     domain.FeatureOnOff,
			States:  []domain.StateKey{domain.StateOnOff},
			Actions: []domain.ActionKey{domain.ActionTurnOn, domain.ActionTurnOff, domain.ActionToggle},
		})
	}
	if _, ok := d.Attributes["lightLevel"]; ok {
		features = append(features, domain.Feature{
			Key:    domain.FeatureBrightness,
			States: []domain.StateKey{domain.StateLevel},
		})
	}
	return features
}
