package domain

import "time"

// RuleID uniquely identifies a rule.
type RuleID string

// RunMode governs how concurrent invocations of the same rule interact.
type RunMode string

const (
	// RunModeSingle drops new invocations if one is already running.
	RunModeSingle RunMode = "single"
	// RunModeQueued serialises invocations FIFO.
	RunModeQueued RunMode = "queued"
	// RunModeParallel runs invocations concurrently (per-device locks still
	// apply, so device writes still serialise).
	RunModeParallel RunMode = "parallel"
	// RunModeRestart cancels the running invocation and starts a new one.
	RunModeRestart RunMode = "restart"
)

// Rule is a persisted automation. Serialised to/from JSON in storage and
// over the API. Trigger/Condition/Action are polymorphic — see rule_json.go.
type Rule struct {
	ID            RuleID       `json:"id"`
	Name          string       `json:"name"`
	HumanReadable string       `json:"human_readable,omitempty"`
	Enabled       bool         `json:"enabled"`
	Mode          RunMode      `json:"mode"`
	Triggers      []Trigger    `json:"triggers"`
	Conditions    []Condition  `json:"conditions,omitempty"`
	Actions       []RuleAction `json:"actions"`
	CreatedAt     time.Time    `json:"created_at"`
	UpdatedAt     time.Time    `json:"updated_at"`
}

// Trigger is a marker interface implemented by every trigger type.
// See rules/triggers.go for concrete implementations.
type Trigger interface {
	TriggerKind() string
}

// Condition is a marker interface for rule conditions.
type Condition interface {
	ConditionKind() string
}

// RuleAction is a marker interface for rule actions.
type RuleAction interface {
	ActionKind() string
}

// RuleRun records the outcome of one rule invocation.
type RuleRun struct {
	ID          string        `json:"id"`
	RuleID      RuleID        `json:"rule_id"`
	TriggeredAt time.Time     `json:"triggered_at"`
	Status      RuleRunStatus `json:"status"`
	Error       string        `json:"error,omitempty"`
	Duration    time.Duration `json:"duration"`
}

// RuleRunStatus classifies a run outcome.
type RuleRunStatus string

const (
	RuleRunSuccess RuleRunStatus = "success"
	RuleRunFailed  RuleRunStatus = "failed"
	RuleRunSkipped RuleRunStatus = "skipped"
)
