package domain

import (
	"encoding/json"
	"errors"
	"fmt"
)

// This file wires polymorphic JSON (un)marshalling for Trigger/Condition/RuleAction.
// Each concrete type registers a Kind string via one of the Register* funcs
// below (called from init() in the concrete package). Unmarshal dispatches
// on the "kind" tag; Marshal wraps the concrete value with its Kind.

var (
	triggerFactories   = map[string]func() Trigger{}
	conditionFactories = map[string]func() Condition{}
	actionFactories    = map[string]func() RuleAction{}
)

// RegisterTrigger registers a factory for a trigger kind.
// Concrete trigger packages call this from init().
func RegisterTrigger(kind string, factory func() Trigger) {
	triggerFactories[kind] = factory
}

// RegisterCondition registers a factory for a condition kind.
func RegisterCondition(kind string, factory func() Condition) {
	conditionFactories[kind] = factory
}

// RegisterAction registers a factory for a rule-action kind.
func RegisterAction(kind string, factory func() RuleAction) {
	actionFactories[kind] = factory
}

type kindEnvelope struct {
	Kind string          `json:"kind"`
	Data json.RawMessage `json:"data"`
}

// MarshalTrigger produces a JSON envelope {"kind":"cron","data":{...}}.
func MarshalTrigger(t Trigger) ([]byte, error) {
	raw, err := json.Marshal(t)
	if err != nil {
		return nil, err
	}
	return json.Marshal(kindEnvelope{Kind: t.TriggerKind(), Data: raw})
}

// UnmarshalTrigger dispatches on the "kind" tag.
func UnmarshalTrigger(data []byte) (Trigger, error) {
	var env kindEnvelope
	if err := json.Unmarshal(data, &env); err != nil {
		return nil, err
	}
	f, ok := triggerFactories[env.Kind]
	if !ok {
		return nil, fmt.Errorf("unknown trigger kind %q", env.Kind)
	}
	t := f()
	if err := json.Unmarshal(env.Data, t); err != nil {
		return nil, err
	}
	return t, nil
}

// MarshalCondition mirrors MarshalTrigger.
func MarshalCondition(c Condition) ([]byte, error) {
	raw, err := json.Marshal(c)
	if err != nil {
		return nil, err
	}
	return json.Marshal(kindEnvelope{Kind: c.ConditionKind(), Data: raw})
}

// UnmarshalCondition mirrors UnmarshalTrigger.
func UnmarshalCondition(data []byte) (Condition, error) {
	var env kindEnvelope
	if err := json.Unmarshal(data, &env); err != nil {
		return nil, err
	}
	f, ok := conditionFactories[env.Kind]
	if !ok {
		return nil, fmt.Errorf("unknown condition kind %q", env.Kind)
	}
	c := f()
	if err := json.Unmarshal(env.Data, c); err != nil {
		return nil, err
	}
	return c, nil
}

// MarshalAction mirrors MarshalTrigger.
func MarshalAction(a RuleAction) ([]byte, error) {
	raw, err := json.Marshal(a)
	if err != nil {
		return nil, err
	}
	return json.Marshal(kindEnvelope{Kind: a.ActionKind(), Data: raw})
}

// UnmarshalAction mirrors UnmarshalTrigger.
func UnmarshalAction(data []byte) (RuleAction, error) {
	var env kindEnvelope
	if err := json.Unmarshal(data, &env); err != nil {
		return nil, err
	}
	f, ok := actionFactories[env.Kind]
	if !ok {
		return nil, fmt.Errorf("unknown action kind %q", env.Kind)
	}
	a := f()
	if err := json.Unmarshal(env.Data, a); err != nil {
		return nil, err
	}
	return a, nil
}

// --- Rule marshalling ---

type ruleJSON struct {
	ID            RuleID            `json:"id"`
	Name          string            `json:"name"`
	HumanReadable string            `json:"human_readable,omitempty"`
	Enabled       bool              `json:"enabled"`
	Mode          RunMode           `json:"mode"`
	Triggers      []json.RawMessage `json:"triggers"`
	Conditions    []json.RawMessage `json:"conditions,omitempty"`
	Actions       []json.RawMessage `json:"actions"`
	CreatedAt     string            `json:"created_at"`
	UpdatedAt     string            `json:"updated_at"`
}

// MarshalJSON implements custom marshalling for Rule to preserve polymorphism.
func (r Rule) MarshalJSON() ([]byte, error) {
	out := ruleJSON{
		ID:            r.ID,
		Name:          r.Name,
		HumanReadable: r.HumanReadable,
		Enabled:       r.Enabled,
		Mode:          r.Mode,
		CreatedAt:     r.CreatedAt.UTC().Format("2006-01-02T15:04:05.000000000Z"),
		UpdatedAt:     r.UpdatedAt.UTC().Format("2006-01-02T15:04:05.000000000Z"),
	}
	for _, t := range r.Triggers {
		b, err := MarshalTrigger(t)
		if err != nil {
			return nil, fmt.Errorf("marshal trigger: %w", err)
		}
		out.Triggers = append(out.Triggers, b)
	}
	for _, c := range r.Conditions {
		b, err := MarshalCondition(c)
		if err != nil {
			return nil, fmt.Errorf("marshal condition: %w", err)
		}
		out.Conditions = append(out.Conditions, b)
	}
	for _, a := range r.Actions {
		b, err := MarshalAction(a)
		if err != nil {
			return nil, fmt.Errorf("marshal action: %w", err)
		}
		out.Actions = append(out.Actions, b)
	}
	return json.Marshal(out)
}

// UnmarshalJSON implements custom unmarshalling for Rule.
func (r *Rule) UnmarshalJSON(data []byte) error {
	var in ruleJSON
	if err := json.Unmarshal(data, &in); err != nil {
		return err
	}
	r.ID = in.ID
	r.Name = in.Name
	r.HumanReadable = in.HumanReadable
	r.Enabled = in.Enabled
	r.Mode = in.Mode
	r.Triggers = r.Triggers[:0]
	r.Conditions = r.Conditions[:0]
	r.Actions = r.Actions[:0]

	if in.Mode == "" {
		r.Mode = RunModeSingle
	}
	for _, raw := range in.Triggers {
		t, err := UnmarshalTrigger(raw)
		if err != nil {
			return fmt.Errorf("unmarshal trigger: %w", err)
		}
		r.Triggers = append(r.Triggers, t)
	}
	for _, raw := range in.Conditions {
		c, err := UnmarshalCondition(raw)
		if err != nil {
			return fmt.Errorf("unmarshal condition: %w", err)
		}
		r.Conditions = append(r.Conditions, c)
	}
	for _, raw := range in.Actions {
		a, err := UnmarshalAction(raw)
		if err != nil {
			return fmt.Errorf("unmarshal action: %w", err)
		}
		r.Actions = append(r.Actions, a)
	}
	if len(r.Triggers) == 0 {
		return errors.New("rule has no triggers")
	}
	if len(r.Actions) == 0 {
		return errors.New("rule has no actions")
	}
	return nil
}
