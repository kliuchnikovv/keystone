package domain

import "time"

// Origin describes who initiated a state change or action. Used for audit
// and for rule engines to avoid feedback loops (a rule triggered by its own
// action).
type Origin string

const (
	OriginDeviceReport Origin = "device_report" // transport announced the change
	OriginUserCommand  Origin = "user_command"  // user via UI/API
	OriginRule         Origin = "rule"          // rule engine
	OriginSystem       Origin = "system"        // e.g. initial state fetch
)

// StateSnapshot is a point-in-time reading of one attribute of one feature.
// The Value is intentionally untyped (any) — callers decode based on
// (Feature, Key). Type validation happens at the ingress boundary.
type StateSnapshot struct {
	DeviceID  DeviceID
	Feature   FeatureKey
	Key       StateKey
	Value     any
	UpdatedAt time.Time
	Origin    Origin
}
