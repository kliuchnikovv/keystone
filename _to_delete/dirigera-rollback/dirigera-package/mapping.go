package dirigera

import "github.com/kliuchnikovv/keystone/internal/domain"

// mapDevice converts a DIRIGERA Device into a discovered device + a set of
// (feature, key, value) triples to seed initial state.
type stateEntry struct {
	Feature domain.FeatureKey
	Key     domain.StateKey
	Value   any
}

type mappedDevice struct {
	Type         domain.DeviceType
	Name         string
	Manufacturer string
	Model        string
	Features     []domain.Feature
	States       []stateEntry
	Metadata     map[string]string
	Room         string
}

// mapDIRIGERADevice returns nil for device types we don't yet support
// (controllers, gateways, air-purifiers, water-sensors — TODO in phases).
func mapDIRIGERADevice(d Device) *mappedDevice {
	m := &mappedDevice{
		Manufacturer: d.Attributes.Manufacturer,
		Model:        d.Attributes.Model,
		Metadata: map[string]string{
			"dirigera.id":         d.ID,
			"dirigera.type":       d.Type,
			"dirigera.deviceType": d.DeviceType,
			"dirigera.serial":     d.Attributes.SerialNumber,
			"dirigera.firmware":   d.Attributes.FirmwareVersion,
		},
	}
	if d.Room != nil {
		m.Room = d.Room.Name
	}
	if d.Attributes.CustomName != "" {
		m.Name = d.Attributes.CustomName
	} else if d.Attributes.Model != "" {
		m.Name = d.Attributes.Model
	} else {
		m.Name = d.ID
	}

	switch d.Type {
	case "light":
		return mapLight(m, d)
	case "outlet":
		return mapOutlet(m, d)
	case "motionSensor":
		return mapMotion(m, d)
	case "openCloseSensor":
		return mapContact(m, d)
	case "environmentSensor":
		return mapEnvironment(m, d)
	case "blinds":
		return mapCover(m, d)
	}
	return nil
}

func mapLight(m *mappedDevice, d Device) *mappedDevice {
	m.Type = domain.DeviceTypeLight
	m.Features = []domain.Feature{
		{Key: domain.FeatureOnOff, States: []domain.StateKey{domain.StateOnOff}, Actions: []domain.ActionKey{domain.ActionTurnOn, domain.ActionTurnOff, domain.ActionToggle}},
	}
	if d.Attributes.IsOn != nil {
		m.States = append(m.States, stateEntry{domain.FeatureOnOff, domain.StateOnOff, *d.Attributes.IsOn})
	}
	if d.Attributes.LightLevel != nil {
		m.Features = append(m.Features, domain.Feature{
			Key:     domain.FeatureBrightness,
			States:  []domain.StateKey{domain.StateLevel},
			Actions: []domain.ActionKey{domain.ActionSet},
		})
		m.States = append(m.States, stateEntry{domain.FeatureBrightness, domain.StateLevel, *d.Attributes.LightLevel})
	}
	if d.Attributes.ColorTemperature != nil {
		m.Features = append(m.Features, domain.Feature{
			Key:     domain.FeatureColorTemp,
			States:  []domain.StateKey{domain.StateColorTempK},
			Actions: []domain.ActionKey{domain.ActionSet},
		})
		m.States = append(m.States, stateEntry{domain.FeatureColorTemp, domain.StateColorTempK, *d.Attributes.ColorTemperature})
	}
	return m
}

func mapOutlet(m *mappedDevice, d Device) *mappedDevice {
	m.Type = domain.DeviceTypePlug
	m.Features = []domain.Feature{
		{Key: domain.FeatureOnOff, States: []domain.StateKey{domain.StateOnOff}, Actions: []domain.ActionKey{domain.ActionTurnOn, domain.ActionTurnOff, domain.ActionToggle}},
	}
	if d.Attributes.IsOn != nil {
		m.States = append(m.States, stateEntry{domain.FeatureOnOff, domain.StateOnOff, *d.Attributes.IsOn})
	}
	if d.Attributes.CurrentActivePower != nil || d.Attributes.TotalEnergyConsumed != nil {
		m.Features = append(m.Features, domain.Feature{
			Key:    domain.FeaturePowerMeter,
			States: []domain.StateKey{domain.StatePowerNow, domain.StateEnergyTotal},
		})
		if d.Attributes.CurrentActivePower != nil {
			m.States = append(m.States, stateEntry{domain.FeaturePowerMeter, domain.StatePowerNow, float32(*d.Attributes.CurrentActivePower)})
		}
		if d.Attributes.TotalEnergyConsumed != nil {
			m.States = append(m.States, stateEntry{domain.FeaturePowerMeter, domain.StateEnergyTotal, *d.Attributes.TotalEnergyConsumed})
		}
	}
	return m
}

func mapMotion(m *mappedDevice, d Device) *mappedDevice {
	m.Type = domain.DeviceTypeMotion
	m.Features = []domain.Feature{
		{Key: domain.FeatureMotion, States: []domain.StateKey{domain.StateOccupied}, Events: []domain.EventKey{domain.EventMotionDetected}},
	}
	if d.Attributes.IsDetected != nil {
		m.States = append(m.States, stateEntry{domain.FeatureMotion, domain.StateOccupied, *d.Attributes.IsDetected})
	}
	if d.Attributes.BatteryPercentage != nil {
		m.Features = append(m.Features, domain.Feature{Key: domain.FeatureBattery, States: []domain.StateKey{domain.StateBatteryLvl}})
		m.States = append(m.States, stateEntry{domain.FeatureBattery, domain.StateBatteryLvl, *d.Attributes.BatteryPercentage})
	}
	return m
}

func mapContact(m *mappedDevice, d Device) *mappedDevice {
	m.Type = domain.DeviceTypeContact
	m.Features = []domain.Feature{
		{Key: domain.FeatureContact, States: []domain.StateKey{domain.StateContactOpen}},
	}
	if d.Attributes.IsOpen != nil {
		m.States = append(m.States, stateEntry{domain.FeatureContact, domain.StateContactOpen, *d.Attributes.IsOpen})
	}
	if d.Attributes.BatteryPercentage != nil {
		m.Features = append(m.Features, domain.Feature{Key: domain.FeatureBattery, States: []domain.StateKey{domain.StateBatteryLvl}})
		m.States = append(m.States, stateEntry{domain.FeatureBattery, domain.StateBatteryLvl, *d.Attributes.BatteryPercentage})
	}
	return m
}

func mapEnvironment(m *mappedDevice, d Device) *mappedDevice {
	m.Type = domain.DeviceTypeSensor
	if d.Attributes.CurrentTemperature != nil {
		m.Features = append(m.Features, domain.Feature{Key: domain.FeatureTemperature, States: []domain.StateKey{domain.StateTemperature}})
		m.States = append(m.States, stateEntry{domain.FeatureTemperature, domain.StateTemperature, float32(*d.Attributes.CurrentTemperature)})
	}
	if d.Attributes.CurrentRH != nil {
		m.Features = append(m.Features, domain.Feature{Key: domain.FeatureHumidity, States: []domain.StateKey{domain.StateHumidity}})
		m.States = append(m.States, stateEntry{domain.FeatureHumidity, domain.StateHumidity, float32(*d.Attributes.CurrentRH)})
	}
	if d.Attributes.BatteryPercentage != nil {
		m.Features = append(m.Features, domain.Feature{Key: domain.FeatureBattery, States: []domain.StateKey{domain.StateBatteryLvl}})
		m.States = append(m.States, stateEntry{domain.FeatureBattery, domain.StateBatteryLvl, *d.Attributes.BatteryPercentage})
	}
	return m
}

func mapCover(m *mappedDevice, d Device) *mappedDevice {
	m.Type = domain.DeviceTypeCover
	m.Features = []domain.Feature{
		{Key: domain.FeatureCoverPosition, States: []domain.StateKey{domain.StateLevel}, Actions: []domain.ActionKey{domain.ActionSet}},
	}
	if d.Attributes.BlindsCurrentLevel != nil {
		m.States = append(m.States, stateEntry{domain.FeatureCoverPosition, domain.StateLevel, *d.Attributes.BlindsCurrentLevel})
	}
	return m
}

// featureToAttribute translates (feature, key, value) into DIRIGERA attributes
// map. Used on write path.
func featureToAttribute(feature domain.FeatureKey, key domain.StateKey, value any) (string, any, bool) {
	switch feature {
	case domain.FeatureOnOff:
		if key == domain.StateOnOff {
			b, _ := value.(bool)
			return "isOn", b, true
		}
	case domain.FeatureBrightness:
		if key == domain.StateLevel {
			// DIRIGERA lightLevel is 1..100 (int). Some clients accept 0 for off,
			// but the safe route is to clamp to 1..100 and use isOn separately.
			return "lightLevel", clampLevel(value), true
		}
	case domain.FeatureColorTemp:
		if key == domain.StateColorTempK {
			return "colorTemperature", intValue(value), true
		}
	case domain.FeatureCoverPosition:
		if key == domain.StateLevel {
			return "blindsTargetLevel", clampLevel(value), true
		}
	}
	return "", nil, false
}

func clampLevel(v any) int {
	i := intValue(v)
	if i < 1 {
		return 1
	}
	if i > 100 {
		return 100
	}
	return i
}

func intValue(v any) int {
	switch x := v.(type) {
	case int:
		return x
	case int32:
		return int(x)
	case int64:
		return int(x)
	case float32:
		return int(x)
	case float64:
		return int(x)
	}
	return 0
}

// actionToAttribute maps an action (turn_on/turn_off/toggle) plus current state
// to a DIRIGERA attribute patch.
func actionToAttribute(feature domain.FeatureKey, action domain.ActionKey, currentOn bool) (string, any, bool) {
	if feature != domain.FeatureOnOff {
		return "", nil, false
	}
	switch action {
	case domain.ActionTurnOn:
		return "isOn", true, true
	case domain.ActionTurnOff:
		return "isOn", false, true
	case domain.ActionToggle:
		return "isOn", !currentOn, true
	}
	return "", nil, false
}
