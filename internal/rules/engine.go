package rules

import (
	"context"
	"errors"
	"log/slog"
	"sync"
	"time"

	"github.com/keystone/keystone/internal/domain"
)

// RuleRepository persists rules. Kept as a subset here so the engine doesn't
// depend on the full ports package.
type RuleRepository interface {
	List(ctx context.Context) ([]*domain.Rule, error)
	Save(ctx context.Context, r *domain.Rule) error
	Delete(ctx context.Context, id domain.RuleID) error
}

// Engine is the rules engine.
// Wire-up:
//  1. Construct with New(...)
//  2. Call LoadFromRepo(ctx) to warm the cache from storage.
//  3. Call Start(ctx) to spin up the ingress goroutines. Blocks until ctx done.
//
// The engine subscribes to state snapshots + events + a time ticker and
// dispatches matching rules through a bounded worker pool.
type Engine struct {
	log      *slog.Logger
	repo     RuleRepository
	reader   StateReader
	executor Executor
	states   <-chan domain.StateSnapshot
	events   <-chan domain.Event
	workers  int

	mu           sync.RWMutex
	rules        map[domain.RuleID]*domain.Rule
	runningSemas map[domain.RuleID]chan struct{} // for single-mode enforcement

	runs []domain.RuleRun // in-memory history; bounded
	runMu sync.Mutex
}

// Config for the engine.
type Config struct {
	Log      *slog.Logger
	Repo     RuleRepository
	Reader   StateReader
	Executor Executor
	// States/Events channels — supply from the event bus.
	States  <-chan domain.StateSnapshot
	Events  <-chan domain.Event
	Workers int // default 4
}

// New builds an Engine but does not start it.
func New(cfg Config) *Engine {
	if cfg.Log == nil {
		cfg.Log = slog.Default()
	}
	if cfg.Workers <= 0 {
		cfg.Workers = 4
	}
	return &Engine{
		log:          cfg.Log,
		repo:         cfg.Repo,
		reader:       cfg.Reader,
		executor:     cfg.Executor,
		states:       cfg.States,
		events:       cfg.Events,
		workers:      cfg.Workers,
		rules:        make(map[domain.RuleID]*domain.Rule),
		runningSemas: make(map[domain.RuleID]chan struct{}),
	}
}

// LoadFromRepo hydrates the in-memory rule cache from storage.
func (e *Engine) LoadFromRepo(ctx context.Context) error {
	rules, err := e.repo.List(ctx)
	if err != nil {
		return err
	}
	e.mu.Lock()
	defer e.mu.Unlock()
	e.rules = make(map[domain.RuleID]*domain.Rule, len(rules))
	for _, r := range rules {
		e.rules[r.ID] = r
	}
	e.log.Info("rules loaded", "count", len(rules))
	return nil
}

// Upsert adds or replaces a rule and persists it.
func (e *Engine) Upsert(ctx context.Context, r *domain.Rule) error {
	if r.ID == "" {
		r.ID = domain.RuleID(domain.NewID())
	}
	if r.CreatedAt.IsZero() {
		r.CreatedAt = time.Now().UTC()
	}
	r.UpdatedAt = time.Now().UTC()
	if r.Mode == "" {
		r.Mode = domain.RunModeSingle
	}
	if err := e.repo.Save(ctx, r); err != nil {
		return err
	}
	e.mu.Lock()
	e.rules[r.ID] = r
	e.mu.Unlock()
	e.log.Info("rule upserted", "id", r.ID, "name", r.Name, "enabled", r.Enabled)
	return nil
}

// Remove deletes a rule.
func (e *Engine) Remove(ctx context.Context, id domain.RuleID) error {
	if err := e.repo.Delete(ctx, id); err != nil {
		return err
	}
	e.mu.Lock()
	delete(e.rules, id)
	delete(e.runningSemas, id)
	e.mu.Unlock()
	return nil
}

// List returns all rules (copies).
func (e *Engine) List() []*domain.Rule {
	e.mu.RLock()
	defer e.mu.RUnlock()
	out := make([]*domain.Rule, 0, len(e.rules))
	for _, r := range e.rules {
		cp := *r
		out = append(out, &cp)
	}
	return out
}

// Runs returns the last N rule invocations (most recent first).
func (e *Engine) Runs(limit int) []domain.RuleRun {
	e.runMu.Lock()
	defer e.runMu.Unlock()
	if limit <= 0 || limit > len(e.runs) {
		limit = len(e.runs)
	}
	out := make([]domain.RuleRun, limit)
	// Runs are appended chronologically; return most recent first.
	for i := 0; i < limit; i++ {
		out[i] = e.runs[len(e.runs)-1-i]
	}
	return out
}

// Start blocks and runs the engine until ctx is cancelled.
func (e *Engine) Start(ctx context.Context) error {
	if e.states == nil && e.events == nil {
		return errors.New("engine has no ingress channels")
	}
	e.log.Info("rules engine started", "workers", e.workers)

	// Time ticker fires every second so TimeTriggers with 1-second granularity
	// work. Real cost is minimal at 1Hz.
	ticker := time.NewTicker(1 * time.Second)
	defer ticker.Stop()

	dispatch := make(chan ruleInvocation, 256)

	// Worker pool.
	var workerWG sync.WaitGroup
	for i := 0; i < e.workers; i++ {
		workerWG.Add(1)
		go func(id int) {
			defer workerWG.Done()
			e.workerLoop(ctx, id, dispatch)
		}(i)
	}

	for {
		select {
		case <-ctx.Done():
			close(dispatch)
			workerWG.Wait()
			e.log.Info("rules engine stopped")
			return nil

		case now := <-ticker.C:
			e.dispatchTimeTriggers(now, dispatch)

		case s, ok := <-e.states:
			if !ok {
				e.states = nil
				continue
			}
			e.dispatchStateTriggers(s, dispatch)

		case ev, ok := <-e.events:
			if !ok {
				e.events = nil
				continue
			}
			e.dispatchEventTriggers(ev, dispatch)
		}
	}
}

type ruleInvocation struct {
	Rule   *domain.Rule
	Reason string
}

func (e *Engine) dispatchTimeTriggers(now time.Time, out chan<- ruleInvocation) {
	e.mu.RLock()
	defer e.mu.RUnlock()
	for _, r := range e.rules {
		if !r.Enabled {
			continue
		}
		for _, t := range r.Triggers {
			tt, ok := t.(*TimeTrigger)
			if !ok {
				continue
			}
			if tt.Matches(now) {
				select {
				case out <- ruleInvocation{Rule: r, Reason: "time trigger"}:
				default:
					e.log.Warn("dispatch queue full, dropping time trigger", "rule", r.ID)
				}
				break
			}
		}
	}
}

func (e *Engine) dispatchStateTriggers(s domain.StateSnapshot, out chan<- ruleInvocation) {
	e.mu.RLock()
	defer e.mu.RUnlock()
	for _, r := range e.rules {
		if !r.Enabled {
			continue
		}
		for _, t := range r.Triggers {
			matched := false
			switch tt := t.(type) {
			case *StateChangedTrigger:
				matched = tt.Matches(s)
			case *StateEqualsTrigger:
				matched = tt.Matches(s)
			case *ThresholdTrigger:
				matched = tt.Matches(s)
			}
			if matched {
				select {
				case out <- ruleInvocation{Rule: r, Reason: "state trigger"}:
				default:
					e.log.Warn("dispatch queue full, dropping state trigger", "rule", r.ID)
				}
				break
			}
		}
	}
}

func (e *Engine) dispatchEventTriggers(ev domain.Event, out chan<- ruleInvocation) {
	e.mu.RLock()
	defer e.mu.RUnlock()
	for _, r := range e.rules {
		if !r.Enabled {
			continue
		}
		for _, t := range r.Triggers {
			et, ok := t.(*EventTrigger)
			if !ok {
				continue
			}
			if et.Matches(ev) {
				select {
				case out <- ruleInvocation{Rule: r, Reason: "event trigger"}:
				default:
					e.log.Warn("dispatch queue full, dropping event trigger", "rule", r.ID)
				}
				break
			}
		}
	}
}

func (e *Engine) workerLoop(ctx context.Context, id int, in <-chan ruleInvocation) {
	for inv := range in {
		if inv.Rule == nil {
			continue
		}
		e.runRule(ctx, inv.Rule, inv.Reason)
	}
	_ = id
}

func (e *Engine) runRule(ctx context.Context, r *domain.Rule, reason string) {
	start := time.Now()
	log := e.log.With("rule_id", r.ID, "rule_name", r.Name, "reason", reason)

	// Enforce RunMode.
	if !e.tryStart(r) {
		log.Debug("rule invocation skipped (mode)", "mode", r.Mode)
		e.recordRun(r.ID, start, domain.RuleRunSkipped, "already running in "+string(r.Mode)+" mode", 0)
		return
	}
	defer e.finish(r.ID)

	// Evaluate conditions (AND).
	for _, c := range r.Conditions {
		if !e.evaluateCondition(c) {
			log.Debug("rule condition failed", "condition", c.ConditionKind())
			e.recordRun(r.ID, start, domain.RuleRunSkipped, "condition failed: "+c.ConditionKind(), time.Since(start))
			return
		}
	}

	// Execute actions in order.
	for i, a := range r.Actions {
		if err := e.executeAction(ctx, a); err != nil {
			log.Error("rule action failed", "index", i, "kind", a.ActionKind(), "err", err)
			e.recordRun(r.ID, start, domain.RuleRunFailed, err.Error(), time.Since(start))
			return
		}
	}
	log.Info("rule executed", "duration_ms", time.Since(start).Milliseconds())
	e.recordRun(r.ID, start, domain.RuleRunSuccess, "", time.Since(start))
}

func (e *Engine) tryStart(r *domain.Rule) bool {
	e.mu.Lock()
	defer e.mu.Unlock()

	sema, ok := e.runningSemas[r.ID]
	if !ok {
		// Slot size depends on mode.
		size := 1
		if r.Mode == domain.RunModeParallel {
			size = 8
		}
		sema = make(chan struct{}, size)
		e.runningSemas[r.ID] = sema
	}
	select {
	case sema <- struct{}{}:
		return true
	default:
		return false
	}
}

func (e *Engine) finish(id domain.RuleID) {
	e.mu.RLock()
	sema := e.runningSemas[id]
	e.mu.RUnlock()
	if sema != nil {
		<-sema
	}
}

func (e *Engine) evaluateCondition(c domain.Condition) bool {
	switch cc := c.(type) {
	case *StateEqualsCondition:
		return cc.Evaluate(e.reader)
	case *TimeRangeCondition:
		return cc.Evaluate(e.reader)
	}
	return false
}

func (e *Engine) executeAction(ctx context.Context, a domain.RuleAction) error {
	switch aa := a.(type) {
	case *InvokeAction:
		return aa.Execute(ctx, e.executor)
	case *SetState:
		return aa.Execute(ctx, e.executor)
	case *Delay:
		return aa.Execute(ctx, e.executor)
	}
	return errors.New("unknown action kind")
}

func (e *Engine) recordRun(id domain.RuleID, at time.Time, status domain.RuleRunStatus, errStr string, dur time.Duration) {
	e.runMu.Lock()
	defer e.runMu.Unlock()
	e.runs = append(e.runs, domain.RuleRun{
		ID:          domain.NewID(),
		RuleID:      id,
		TriggeredAt: at.UTC(),
		Status:      status,
		Error:       errStr,
		Duration:    dur,
	})
	// Bound history to last 500.
	if len(e.runs) > 500 {
		e.runs = e.runs[len(e.runs)-500:]
	}
}
