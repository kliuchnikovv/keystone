package rules

import (
	"context"
	"log/slog"
	"os"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"github.com/kliuchnikovv/keystone/internal/domain"
)

// mockRepo satisfies rules.RuleRepository in tests without touching disk.
type mockRepo struct {
	mu    sync.Mutex
	rules map[domain.RuleID]*domain.Rule
}

func newMockRepo() *mockRepo {
	return &mockRepo{rules: map[domain.RuleID]*domain.Rule{}}
}

func (m *mockRepo) List(ctx context.Context) ([]*domain.Rule, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	out := make([]*domain.Rule, 0, len(m.rules))
	for _, r := range m.rules {
		out = append(out, r)
	}
	return out, nil
}

func (m *mockRepo) Save(ctx context.Context, r *domain.Rule) error {
	m.mu.Lock()
	defer m.mu.Unlock()
	m.rules[r.ID] = r
	return nil
}

func (m *mockRepo) Delete(ctx context.Context, id domain.RuleID) error {
	m.mu.Lock()
	defer m.mu.Unlock()
	delete(m.rules, id)
	return nil
}

type mockExecutor struct {
	invocations int32
	lastAction  domain.ActionKey
}

func (m *mockExecutor) InvokeAction(_ context.Context, _ domain.DeviceID, _ domain.FeatureKey, action domain.ActionKey, _ map[string]any) error {
	atomic.AddInt32(&m.invocations, 1)
	m.lastAction = action
	return nil
}

func (m *mockExecutor) WriteState(_ context.Context, _ domain.DeviceID, _ domain.FeatureKey, _ domain.StateKey, _ any) error {
	atomic.AddInt32(&m.invocations, 1)
	return nil
}

type mockReader struct{}

func (mockReader) GetState(_ domain.DeviceID, _ domain.FeatureKey, _ domain.StateKey) (domain.StateSnapshot, bool) {
	return domain.StateSnapshot{}, false
}

// TestEngine_ThresholdTriggerFiresAction verifies that a threshold trigger
// dispatches to the action executor when a matching state snapshot arrives.
func TestEngine_ThresholdTriggerFiresAction(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	repo := newMockRepo()
	ex := &mockExecutor{}
	states := make(chan domain.StateSnapshot, 4)
	events := make(chan domain.Event, 4)

	eng := New(Config{
		Log:      slog.New(slog.NewTextHandler(os.Stderr, &slog.HandlerOptions{Level: slog.LevelError})),
		Repo:     repo,
		Reader:   mockReader{},
		Executor: ex,
		States:   states,
		Events:   events,
		Workers:  2,
	})

	rule := &domain.Rule{
		Name:    "test",
		Enabled: true,
		Mode:    domain.RunModeSingle,
		Triggers: []domain.Trigger{
			&ThresholdTrigger{
				DeviceID: "d1",
				Feature:  domain.FeaturePowerMeter,
				Key:      domain.StatePowerNow,
				Op:       "gt",
				Value:    1000,
			},
		},
		Actions: []domain.RuleAction{
			&InvokeAction{DeviceID: "d2", Feature: domain.FeatureOnOff, Action: domain.ActionTurnOn},
		},
	}
	if err := eng.Upsert(ctx, rule); err != nil {
		t.Fatal(err)
	}

	go func() { _ = eng.Start(ctx) }()

	// Below threshold — no fire.
	states <- domain.StateSnapshot{DeviceID: "d1", Feature: domain.FeaturePowerMeter, Key: domain.StatePowerNow, Value: float32(500)}
	time.Sleep(50 * time.Millisecond)
	if got := atomic.LoadInt32(&ex.invocations); got != 0 {
		t.Fatalf("expected 0 invocations, got %d", got)
	}

	// Above threshold — one fire.
	states <- domain.StateSnapshot{DeviceID: "d1", Feature: domain.FeaturePowerMeter, Key: domain.StatePowerNow, Value: float32(1500)}
	time.Sleep(50 * time.Millisecond)
	if got := atomic.LoadInt32(&ex.invocations); got != 1 {
		t.Fatalf("expected 1 invocation, got %d", got)
	}
	if ex.lastAction != domain.ActionTurnOn {
		t.Fatalf("expected turn_on, got %s", ex.lastAction)
	}
}

// TestEngine_TimeRangeCondition verifies that a rule whose condition fails is
// recorded as skipped and doesn't execute actions.
func TestEngine_TimeRangeCondition(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	repo := newMockRepo()
	ex := &mockExecutor{}
	states := make(chan domain.StateSnapshot, 4)
	events := make(chan domain.Event, 4)

	eng := New(Config{
		Log: slog.New(slog.NewTextHandler(os.Stderr, &slog.HandlerOptions{Level: slog.LevelError})),
		Repo:     repo,
		Reader:   mockReader{},
		Executor: ex,
		States:   states,
		Events:   events,
		Workers:  2,
	})

	// Condition that never matches (0:00–0:01 UTC), so trigger fires but rule is skipped.
	rule := &domain.Rule{
		Name:    "conditional",
		Enabled: true,
		Mode:    domain.RunModeSingle,
		Triggers: []domain.Trigger{
			&StateEqualsTrigger{DeviceID: "d1", Feature: domain.FeatureOnOff, Key: domain.StateOnOff, Value: true},
		},
		Conditions: []domain.Condition{
			&TimeRangeCondition{From: "00:00", To: "00:01"},
		},
		Actions: []domain.RuleAction{
			&InvokeAction{DeviceID: "d2", Feature: domain.FeatureOnOff, Action: domain.ActionTurnOn},
		},
	}
	if err := eng.Upsert(ctx, rule); err != nil {
		t.Fatal(err)
	}

	go func() { _ = eng.Start(ctx) }()

	states <- domain.StateSnapshot{DeviceID: "d1", Feature: domain.FeatureOnOff, Key: domain.StateOnOff, Value: true}
	time.Sleep(50 * time.Millisecond)

	if got := atomic.LoadInt32(&ex.invocations); got != 0 {
		t.Fatalf("expected 0 invocations (condition blocks), got %d", got)
	}

	runs := eng.Runs(10)
	if len(runs) != 1 {
		t.Fatalf("expected 1 run recorded, got %d", len(runs))
	}
	if runs[0].Status != domain.RuleRunSkipped {
		t.Fatalf("expected skipped, got %s", runs[0].Status)
	}
}
