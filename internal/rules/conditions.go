package rules

import (
	"time"

	"github.com/kliuchnikovv/keystone/internal/domain"
)

func init() {
	domain.RegisterCondition("state_equals", func() domain.Condition { return &StateEqualsCondition{} })
	domain.RegisterCondition("time_range", func() domain.Condition { return &TimeRangeCondition{} })
}

// StateReader is what conditions use to inspect current state.
type StateReader interface {
	GetState(id domain.DeviceID, feature domain.FeatureKey, key domain.StateKey) (domain.StateSnapshot, bool)
}

// StateEqualsCondition passes if a state attribute equals Value at eval time.
type StateEqualsCondition struct {
	DeviceID domain.DeviceID   `json:"device_id"`
	Feature  domain.FeatureKey `json:"feature"`
	Key      domain.StateKey   `json:"key"`
	Value    any               `json:"value"`
}

// ConditionKind implements domain.Condition.
func (c *StateEqualsCondition) ConditionKind() string { return "state_equals" }

// Evaluate looks up the current cached value.
func (c *StateEqualsCondition) Evaluate(r StateReader) bool {
	s, ok := r.GetState(c.DeviceID, c.Feature, c.Key)
	if !ok {
		return false
	}
	return equal(s.Value, c.Value)
}

// TimeRangeCondition passes if the current local time falls within [From, To).
// Timezone defaults to UTC if empty. From/To are "HH:MM"; if To < From the
// range wraps midnight (e.g. From="22:00" To="06:00").
type TimeRangeCondition struct {
	From     string `json:"from"`
	To       string `json:"to"`
	Timezone string `json:"timezone,omitempty"`
}

// ConditionKind implements domain.Condition.
func (c *TimeRangeCondition) ConditionKind() string { return "time_range" }

// Evaluate at time now.
func (c *TimeRangeCondition) Evaluate(_ StateReader) bool {
	return c.EvaluateAt(time.Now())
}

// EvaluateAt evaluates at an arbitrary instant (used by tests).
func (c *TimeRangeCondition) EvaluateAt(now time.Time) bool {
	loc := time.UTC
	if c.Timezone != "" {
		if l, err := time.LoadLocation(c.Timezone); err == nil {
			loc = l
		}
	}
	local := now.In(loc)

	fh, fm, fok := parseHHMM(c.From)
	th, tm, tok := parseHHMM(c.To)
	if !fok || !tok {
		return false
	}
	fromMin := fh*60 + fm
	toMin := th*60 + tm
	curMin := local.Hour()*60 + local.Minute()

	if fromMin <= toMin {
		return curMin >= fromMin && curMin < toMin
	}
	// Wraps midnight
	return curMin >= fromMin || curMin < toMin
}
