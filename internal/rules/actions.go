package rules

import (
	"context"
	"time"

	"github.com/keystone/keystone/internal/domain"
)

func init() {
	domain.RegisterAction("invoke_action", func() domain.RuleAction { return &InvokeAction{} })
	domain.RegisterAction("set_state", func() domain.RuleAction { return &SetState{} })
	domain.RegisterAction("delay", func() domain.RuleAction { return &Delay{} })
}

// Executor is the subset of the device service that actions call.
type Executor interface {
	InvokeAction(ctx context.Context, id domain.DeviceID, feature domain.FeatureKey, action domain.ActionKey, params map[string]any) error
	WriteState(ctx context.Context, id domain.DeviceID, feature domain.FeatureKey, key domain.StateKey, value any) error
}

// InvokeAction calls a device action.
type InvokeAction struct {
	DeviceID domain.DeviceID   `json:"device_id"`
	Feature  domain.FeatureKey `json:"feature"`
	Action   domain.ActionKey  `json:"action"`
	Params   map[string]any    `json:"params,omitempty"`
}

// ActionKind implements domain.RuleAction.
func (a *InvokeAction) ActionKind() string { return "invoke_action" }

// Execute runs the action against the executor.
func (a *InvokeAction) Execute(ctx context.Context, ex Executor) error {
	return ex.InvokeAction(ctx, a.DeviceID, a.Feature, a.Action, a.Params)
}

// SetState writes a specific attribute value.
type SetState struct {
	DeviceID domain.DeviceID   `json:"device_id"`
	Feature  domain.FeatureKey `json:"feature"`
	Key      domain.StateKey   `json:"key"`
	Value    any               `json:"value"`
}

// ActionKind implements domain.RuleAction.
func (a *SetState) ActionKind() string { return "set_state" }

// Execute runs the action against the executor.
func (a *SetState) Execute(ctx context.Context, ex Executor) error {
	return ex.WriteState(ctx, a.DeviceID, a.Feature, a.Key, a.Value)
}

// Delay pauses rule execution.
type Delay struct {
	Duration    time.Duration `json:"duration"`
	DurationStr string        `json:"duration_str,omitempty"`
}

// ActionKind implements domain.RuleAction.
func (a *Delay) ActionKind() string { return "delay" }

// Execute pauses for Duration or until context cancellation.
func (a *Delay) Execute(ctx context.Context, _ Executor) error {
	select {
	case <-ctx.Done():
		return ctx.Err()
	case <-time.After(a.Duration):
		return nil
	}
}
