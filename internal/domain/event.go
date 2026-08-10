package domain

import "time"

// Event is an emitted message about something that happened. Distinct from
// state changes: a button-press event fires without changing any persistent
// state.
type Event struct {
	DeviceID DeviceID
	Feature  FeatureKey
	Name     EventKey
	Data     map[string]any
	At       time.Time
}

// Action is an imperative command directed at a device+feature.
type Action struct {
	DeviceID DeviceID
	Feature  FeatureKey
	Name     ActionKey
	Params   map[string]any
	Issuer   Origin
}
