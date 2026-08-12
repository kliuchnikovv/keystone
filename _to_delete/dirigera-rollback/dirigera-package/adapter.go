package dirigera

import (
	"context"
	"errors"
	"fmt"
	"log/slog"
	"sync"
	"time"

	"github.com/kliuchnikovv/keystone/internal/domain"
	"github.com/kliuchnikovv/keystone/internal/ports"
)

// Adapter implements ports.Adapter against an IKEA DIRIGERA hub.
//
// MVP implementation:
//   - Uses HTTPS REST for discover/read/write.
//   - Polls state every 2 seconds; diffs snapshots to emit state_changed events.
//   - No WebSocket yet (planned follow-up).
type Adapter struct {
	log    *slog.Logger
	cfg    *Config
	client *Client

	pollInterval time.Duration

	mu          sync.Mutex
	started     bool
	lastSnap    map[string]Device // dirigera-id → last-known state
	discovered  map[string]bool   // dirigera-id → true if already reported via Discover

	subMu sync.RWMutex
	subs  map[chan ports.TransportEvent]struct{}
}

// New builds an unconfigured adapter. Call LoadFromDir before Start.
func New(log *slog.Logger) *Adapter {
	if log == nil {
		log = slog.Default()
	}
	return &Adapter{
		log:          log.With("component", "dirigera"),
		pollInterval: 2 * time.Second,
		lastSnap:     make(map[string]Device),
		discovered:   make(map[string]bool),
		subs:         make(map[chan ports.TransportEvent]struct{}),
	}
}

// LoadFromDir reads dirigera.json from the given data directory.
// Returns nil (no error) if the file is missing — caller must decide whether
// that's acceptable (e.g. adapter disabled) or should error out.
func (a *Adapter) LoadFromDir(dataDir string) (bool, error) {
	c, err := LoadConfig(dataDir)
	if err != nil {
		if errors.Is(err, errNotExist(err)) || err.Error() == "open "+dataDir+"/dirigera.json: no such file or directory" {
			return false, nil
		}
		// Try to distinguish "missing file" more portably.
		if isNotExist(err) {
			return false, nil
		}
		return false, err
	}
	a.cfg = c
	a.client = NewClient(c.Host, c.Token, 10*time.Second)
	return true, nil
}

// Kind implements ports.Adapter.
func (a *Adapter) Kind() domain.TransportKind { return domain.TransportDirigera }

// Start pings the hub and begins the poll loop.
func (a *Adapter) Start(ctx context.Context) error {
	a.mu.Lock()
	defer a.mu.Unlock()
	if a.started {
		return errors.New("dirigera adapter already started")
	}
	if a.client == nil {
		return errors.New("dirigera adapter not configured; run keystone-dirigera-pair first")
	}
	if err := a.client.HubStatus(ctx); err != nil {
		return fmt.Errorf("dirigera hub unreachable: %w", err)
	}
	a.started = true
	a.log.Info("dirigera adapter started", "host", a.cfg.Host)

	go a.pollLoop(ctx)
	return nil
}

// Stop implements ports.Adapter.
func (a *Adapter) Stop(ctx context.Context) error {
	a.mu.Lock()
	defer a.mu.Unlock()
	a.started = false

	a.subMu.Lock()
	for ch := range a.subs {
		close(ch)
		delete(a.subs, ch)
	}
	a.subMu.Unlock()

	a.log.Info("dirigera adapter stopped")
	return nil
}

// Discover implements ports.Adapter. On call — fetches current device list.
// Devices already seen (by dirigera-id) are still emitted on this call so the
// core can rehydrate its registry.
func (a *Adapter) Discover(ctx context.Context) (<-chan ports.DiscoveredDevice, error) {
	devices, err := a.client.ListDevices(ctx)
	if err != nil {
		return nil, fmt.Errorf("list devices: %w", err)
	}
	out := make(chan ports.DiscoveredDevice, len(devices))
	for _, d := range devices {
		if m := mapDIRIGERADevice(d); m != nil {
			out <- ports.DiscoveredDevice{
				TransportRef: domain.TransportRef(d.ID),
				Type:         m.Type,
				Name:         m.Name,
				Manufacturer: m.Manufacturer,
				Model:        m.Model,
				Features:     m.Features,
				Metadata:     m.Metadata,
			}
			a.mu.Lock()
			a.discovered[d.ID] = true
			a.lastSnap[d.ID] = d
			a.mu.Unlock()
		}
	}
	close(out)
	return out, nil
}

// Commission is not applicable for DIRIGERA — devices are paired through the
// IKEA app. In future we may wrap the pair-new-device flow via the hub API.
func (a *Adapter) Commission(ctx context.Context, req ports.CommissionRequest) (domain.TransportRef, error) {
	return "", errors.New("dirigera adapter: commissioning happens via the IKEA app, not through keystone yet")
}

// ReadState implements ports.Adapter — pulls the latest device state and
// extracts the requested attribute.
func (a *Adapter) ReadState(ctx context.Context, ref domain.TransportRef, feature domain.FeatureKey, key domain.StateKey) (any, error) {
	d, err := a.client.GetDevice(ctx, string(ref))
	if err != nil {
		return nil, err
	}
	m := mapDIRIGERADevice(*d)
	if m == nil {
		return nil, fmt.Errorf("unsupported device type %q", d.Type)
	}
	for _, s := range m.States {
		if s.Feature == feature && s.Key == key {
			return s.Value, nil
		}
	}
	return nil, fmt.Errorf("state %s.%s not present", feature, key)
}

// WriteState implements ports.Adapter.
func (a *Adapter) WriteState(ctx context.Context, ref domain.TransportRef, feature domain.FeatureKey, key domain.StateKey, value any) error {
	attr, val, ok := featureToAttribute(feature, key, value)
	if !ok {
		return fmt.Errorf("dirigera: cannot map feature %s.%s to attribute", feature, key)
	}
	return a.client.PatchDeviceAttributes(ctx, string(ref), map[string]any{attr: val})
}

// InvokeAction implements ports.Adapter.
func (a *Adapter) InvokeAction(ctx context.Context, ref domain.TransportRef, feature domain.FeatureKey, action domain.ActionKey, params map[string]any) error {
	// For toggle we need current state — cheap enough to fetch.
	var currentOn bool
	if action == domain.ActionToggle {
		d, err := a.client.GetDevice(ctx, string(ref))
		if err != nil {
			return err
		}
		if d.Attributes.IsOn != nil {
			currentOn = *d.Attributes.IsOn
		}
	}
	attr, val, ok := actionToAttribute(feature, action, currentOn)
	if !ok {
		// Fall through: generic set — expect params to carry the attribute value
		if action == domain.ActionSet && params != nil {
			body := map[string]any{}
			for k, v := range params {
				body[k] = v
			}
			return a.client.PatchDeviceAttributes(ctx, string(ref), body)
		}
		return fmt.Errorf("dirigera: unsupported action %s on %s", action, feature)
	}
	return a.client.PatchDeviceAttributes(ctx, string(ref), map[string]any{attr: val})
}

// Subscribe returns a channel that receives transport events.
func (a *Adapter) Subscribe(ctx context.Context) (<-chan ports.TransportEvent, error) {
	ch := make(chan ports.TransportEvent, 128)
	a.subMu.Lock()
	a.subs[ch] = struct{}{}
	a.subMu.Unlock()

	go func() {
		<-ctx.Done()
		a.subMu.Lock()
		if _, ok := a.subs[ch]; ok {
			delete(a.subs, ch)
			close(ch)
		}
		a.subMu.Unlock()
	}()
	return ch, nil
}

// Decommission is not applicable — user removes via IKEA app.
func (a *Adapter) Decommission(ctx context.Context, ref domain.TransportRef) error {
	return errors.New("dirigera adapter: unpair via the IKEA app")
}

// ---- polling ----

func (a *Adapter) pollLoop(ctx context.Context) {
	// One immediate poll so state is fresh right after startup.
	a.pollOnce(ctx)

	t := time.NewTicker(a.pollInterval)
	defer t.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case <-t.C:
			a.pollOnce(ctx)
		}
	}
}

func (a *Adapter) pollOnce(ctx context.Context) {
	devices, err := a.client.ListDevices(ctx)
	if err != nil {
		a.log.Warn("poll: list devices failed", "err", err)
		return
	}
	now := make(map[string]Device, len(devices))
	for _, d := range devices {
		now[d.ID] = d
	}

	a.mu.Lock()
	prev := a.lastSnap
	a.lastSnap = now
	a.mu.Unlock()

	// Emit state_changed for each observed diff.
	for id, d := range now {
		p, seen := prev[id]
		if !seen {
			// New device; core rescans via Discover later.
			continue
		}
		emitDiff(a, id, p, d)

		// Reachability toggles as online/offline events.
		if p.IsReachable != d.IsReachable {
			kind := ports.TransportEventOnline
			if !d.IsReachable {
				kind = ports.TransportEventOffline
			}
			a.emit(ports.TransportEvent{Ref: domain.TransportRef(id), Kind: kind})
		}
	}
}

func emitDiff(a *Adapter, id string, prev, curr Device) {
	prevMapped := mapDIRIGERADevice(prev)
	currMapped := mapDIRIGERADevice(curr)
	if prevMapped == nil || currMapped == nil {
		return
	}
	// Index prev by (feature, key).
	prevIdx := map[[2]string]any{}
	for _, s := range prevMapped.States {
		prevIdx[[2]string{string(s.Feature), string(s.Key)}] = s.Value
	}
	for _, s := range currMapped.States {
		k := [2]string{string(s.Feature), string(s.Key)}
		p, seen := prevIdx[k]
		if seen && equalAny(p, s.Value) {
			continue
		}
		a.emit(ports.TransportEvent{
			Ref:     domain.TransportRef(id),
			Kind:    ports.TransportEventStateChanged,
			Feature: s.Feature,
			Key:     string(s.Key),
			Value:   s.Value,
		})
	}
}

func equalAny(a, b any) bool {
	// Fast path for common types.
	switch av := a.(type) {
	case bool:
		bv, ok := b.(bool)
		return ok && av == bv
	case int:
		bv, ok := b.(int)
		return ok && av == bv
	case float32:
		bv, ok := b.(float32)
		return ok && av == bv
	case float64:
		bv, ok := b.(float64)
		return ok && av == bv
	case string:
		bv, ok := b.(string)
		return ok && av == bv
	}
	return fmt.Sprintf("%v", a) == fmt.Sprintf("%v", b)
}

func (a *Adapter) emit(ev ports.TransportEvent) {
	a.subMu.RLock()
	defer a.subMu.RUnlock()
	for ch := range a.subs {
		select {
		case ch <- ev:
		default:
			select {
			case <-ch:
			default:
			}
			select {
			case ch <- ev:
			default:
			}
		}
	}
}

// isNotExist is os.IsNotExist tolerating wrapped errors.
func isNotExist(err error) bool {
	for e := err; e != nil; {
		if unwrapped, ok := e.(interface{ Unwrap() error }); ok {
			e = unwrapped.Unwrap()
			continue
		}
		break
	}
	// Fallback to substring match; os.IsNotExist doesn't unwrap fmt.Errorf.
	s := err.Error()
	return contains(s, "no such file") || contains(s, "cannot find the file")
}

func contains(s, sub string) bool {
	for i := 0; i+len(sub) <= len(s); i++ {
		if s[i:i+len(sub)] == sub {
			return true
		}
	}
	return false
}

// errNotExist is a small helper used in LoadFromDir to keep imports minimal.
func errNotExist(err error) error { return err }
