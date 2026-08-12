// Package rules implements the automation engine: rule storage, trigger
// matching, condition evaluation, and action execution.
package rules

import (
	"fmt"
	"strings"
	"time"

	"github.com/kliuchnikovv/keystone/internal/domain"
)

func init() {
	domain.RegisterTrigger("time", func() domain.Trigger { return &TimeTrigger{} })
	domain.RegisterTrigger("state_changed", func() domain.Trigger { return &StateChangedTrigger{} })
	domain.RegisterTrigger("state_equals", func() domain.Trigger { return &StateEqualsTrigger{} })
	domain.RegisterTrigger("threshold", func() domain.Trigger { return &ThresholdTrigger{} })
	domain.RegisterTrigger("event", func() domain.Trigger { return &EventTrigger{} })
}

// TimeTrigger fires on a schedule. Supports three modes:
//   - Daily "HH:MM": every day at that local time
//   - Weekly:       Days ["mon","wed"], TimeOfDay "HH:MM"
//   - Every:        interval like "5m", "1h"
type TimeTrigger struct {
	Daily      string        `json:"daily,omitempty"`  // "HH:MM"
	Days       []string      `json:"days,omitempty"`   // ["mon","wed","fri"]
	TimeOfDay  string        `json:"time_of_day,omitempty"`
	Every      time.Duration `json:"every,omitempty"`  // parsed by JSON as ns
	EveryStr   string        `json:"every_str,omitempty"` // human "5m" for round-trip
	Timezone   string        `json:"timezone,omitempty"`
}

// TriggerKind implements domain.Trigger.
func (t *TimeTrigger) TriggerKind() string { return "time" }

// Location returns the trigger's timezone, or UTC as fallback.
func (t *TimeTrigger) Location() *time.Location {
	if t.Timezone == "" {
		return time.UTC
	}
	loc, err := time.LoadLocation(t.Timezone)
	if err != nil {
		return time.UTC
	}
	return loc
}

// Matches returns true if the given time is a scheduled tick for this trigger.
// Called by Engine once a minute (or every second for Every-mode < 60s).
func (t *TimeTrigger) Matches(now time.Time) bool {
	local := now.In(t.Location())

	if t.Daily != "" {
		h, m, ok := parseHHMM(t.Daily)
		return ok && local.Hour() == h && local.Minute() == m && local.Second() == 0
	}
	if t.TimeOfDay != "" && len(t.Days) > 0 {
		h, m, ok := parseHHMM(t.TimeOfDay)
		if !ok || local.Hour() != h || local.Minute() != m || local.Second() != 0 {
			return false
		}
		wd := strings.ToLower(local.Weekday().String()[:3])
		for _, d := range t.Days {
			if strings.ToLower(d) == wd {
				return true
			}
		}
		return false
	}
	if t.Every > 0 {
		// Simple: fires when unix-seconds since epoch % Every == 0.
		return int64(now.Unix())%int64(t.Every.Seconds()) == 0
	}
	return false
}

func parseHHMM(s string) (int, int, bool) {
	var h, m int
	if _, err := fmt.Sscanf(s, "%d:%d", &h, &m); err != nil {
		return 0, 0, false
	}
	if h < 0 || h > 23 || m < 0 || m > 59 {
		return 0, 0, false
	}
	return h, m, true
}

// StateChangedTrigger fires when a specific state attribute changes.
// If To is set, matches only when the new value equals To.
type StateChangedTrigger struct {
	DeviceID domain.DeviceID  `json:"device_id"`
	Feature  domain.FeatureKey `json:"feature"`
	Key      domain.StateKey  `json:"key"`
	To       any              `json:"to,omitempty"`
}

// TriggerKind implements domain.Trigger.
func (t *StateChangedTrigger) TriggerKind() string { return "state_changed" }

// Matches returns true if the state snapshot corresponds to this trigger.
func (t *StateChangedTrigger) Matches(s domain.StateSnapshot) bool {
	if s.DeviceID != t.DeviceID || s.Feature != t.Feature || s.Key != t.Key {
		return false
	}
	if t.To == nil {
		return true
	}
	return equal(s.Value, t.To)
}

// StateEqualsTrigger fires on every state snapshot where the value equals
// Value. Semantically similar to StateChangedTrigger with To set, but
// evaluated on every incoming snapshot even if the value didn't actually
// change (useful for periodic sensor reports).
type StateEqualsTrigger struct {
	DeviceID domain.DeviceID   `json:"device_id"`
	Feature  domain.FeatureKey `json:"feature"`
	Key      domain.StateKey   `json:"key"`
	Value    any               `json:"value"`
}

// TriggerKind implements domain.Trigger.
func (t *StateEqualsTrigger) TriggerKind() string { return "state_equals" }

// Matches returns true if the state snapshot equals this trigger's Value.
func (t *StateEqualsTrigger) Matches(s domain.StateSnapshot) bool {
	if s.DeviceID != t.DeviceID || s.Feature != t.Feature || s.Key != t.Key {
		return false
	}
	return equal(s.Value, t.Value)
}

// ThresholdTrigger fires when a numeric state attribute crosses a threshold.
// Op is one of: "gt", "ge", "lt", "le".
type ThresholdTrigger struct {
	DeviceID domain.DeviceID   `json:"device_id"`
	Feature  domain.FeatureKey `json:"feature"`
	Key      domain.StateKey   `json:"key"`
	Op       string            `json:"op"`
	Value    float64           `json:"value"`
}

// TriggerKind implements domain.Trigger.
func (t *ThresholdTrigger) TriggerKind() string { return "threshold" }

// Matches returns true if the state snapshot passes the threshold.
func (t *ThresholdTrigger) Matches(s domain.StateSnapshot) bool {
	if s.DeviceID != t.DeviceID || s.Feature != t.Feature || s.Key != t.Key {
		return false
	}
	v, ok := toFloat(s.Value)
	if !ok {
		return false
	}
	switch t.Op {
	case "gt":
		return v > t.Value
	case "ge":
		return v >= t.Value
	case "lt":
		return v < t.Value
	case "le":
		return v <= t.Value
	}
	return false
}

// EventTrigger fires when a specific event is emitted. If DeviceID or Feature
// is empty, that field is wildcard.
type EventTrigger struct {
	DeviceID domain.DeviceID   `json:"device_id,omitempty"`
	Feature  domain.FeatureKey `json:"feature,omitempty"`
	Name     domain.EventKey   `json:"name"`
}

// TriggerKind implements domain.Trigger.
func (t *EventTrigger) TriggerKind() string { return "event" }

// Matches returns true if the event corresponds to this trigger.
func (t *EventTrigger) Matches(e domain.Event) bool {
	if e.Name != t.Name {
		return false
	}
	if t.DeviceID != "" && e.DeviceID != t.DeviceID {
		return false
	}
	if t.Feature != "" && e.Feature != t.Feature {
		return false
	}
	return true
}

// --- helpers ---

func toFloat(v any) (float64, bool) {
	switch x := v.(type) {
	case float64:
		return x, true
	case float32:
		return float64(x), true
	case int:
		return float64(x), true
	case int32:
		return float64(x), true
	case int64:
		return float64(x), true
	}
	return 0, false
}

func equal(a, b any) bool {
	af, aok := toFloat(a)
	bf, bok := toFloat(b)
	if aok && bok {
		return af == bf
	}
	return fmt.Sprintf("%v", a) == fmt.Sprintf("%v", b)
}
