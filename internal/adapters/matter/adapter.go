package matter

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"log/slog"
	"sync"

	"github.com/kliuchnikovv/keystone/internal/domain"
	"github.com/kliuchnikovv/keystone/internal/ports"
)

// Adapter implements ports.Adapter for Matter devices via a matter.js
// sidecar (WebSocket JSON-RPC). See docs/matter-adapter-brief.md.
//
// This is the Phase 2 skeleton: transport-agnostic (accepts any Client
// implementation), fully wired to the ports.Adapter surface. The concrete
// WebSocket Client and its wire loop land in Phase 1 of the sidecar work.
type Adapter struct {
	log    *slog.Logger
	cfg    Config
	client Client

	mu      sync.Mutex
	started bool

	// Fan-out of TransportEvent to any Subscribe() callers. matter.js only
	// pushes on one WebSocket, so we own a single ingress goroutine and
	// broadcast to N subscribers.
	subMu sync.RWMutex
	subs  map[chan ports.TransportEvent]struct{}

	// cancelIngress stops the goroutine that pumps client.Events() into
	// subscribers. Set on Start, called on Stop.
	cancelIngress context.CancelFunc
}

// New builds a Matter adapter. The Client is injected so tests can supply
// a mock and so the WebSocket implementation can evolve independently.
func New(log *slog.Logger, cfg Config, client Client) *Adapter {
	if log == nil {
		log = slog.Default()
	}
	return &Adapter{
		log:    log,
		cfg:    cfg,
		client: client,
		subs:   make(map[chan ports.TransportEvent]struct{}),
	}
}

// Kind implements ports.Adapter.
func (a *Adapter) Kind() domain.TransportKind { return domain.TransportMatter }

// Start implements ports.Adapter. It dials the sidecar and spins up the
// event pump. If the sidecar is unreachable Start returns an error so main
// can decide whether to abort or continue without Matter.
func (a *Adapter) Start(ctx context.Context) error {
	a.mu.Lock()
	defer a.mu.Unlock()
	if a.started {
		return errors.New("matter adapter already started")
	}
	if a.client == nil {
		return errors.New("matter adapter: nil client")
	}
	if err := a.client.Connect(ctx); err != nil {
		return fmt.Errorf("matter: connect sidecar at %s: %w", a.cfg.URL(), err)
	}
	pumpCtx, cancel := context.WithCancel(context.Background())
	a.cancelIngress = cancel
	go a.pumpEvents(pumpCtx)
	a.started = true
	a.log.Info("matter adapter started", "sidecar", a.cfg.URL())
	return nil
}

// Stop implements ports.Adapter.
func (a *Adapter) Stop(ctx context.Context) error {
	a.mu.Lock()
	defer a.mu.Unlock()
	if !a.started {
		return nil
	}
	a.started = false
	if a.cancelIngress != nil {
		a.cancelIngress()
	}
	err := a.client.Close()
	a.closeAllSubs()
	a.log.Info("matter adapter stopped")
	return err
}

// Discover implements ports.Adapter. It calls listNodes on the sidecar and
// translates each Matter node into a DiscoveredDevice with the Features we
// know how to map. The returned channel is closed after the initial dump.
func (a *Adapter) Discover(ctx context.Context) (<-chan ports.DiscoveredDevice, error) {
	var nodes []Node
	if err := a.client.Call(ctx, MethodListNodes, nil, &nodes); err != nil {
		return nil, fmt.Errorf("matter: listNodes: %w", err)
	}
	out := make(chan ports.DiscoveredDevice, len(nodes))
	for _, n := range nodes {
		out <- nodeToDiscovered(n)
	}
	close(out)
	return out, nil
}

// Commission implements ports.Adapter. For Matter via Apple Home multi-admin,
// req.Payload carries the 11-digit setup code the user copied from
// Home.app's "Turn on Pairing Mode" screen.
func (a *Adapter) Commission(ctx context.Context, req ports.CommissionRequest) (domain.TransportRef, error) {
	if req.Payload == "" {
		return "", errors.New("matter: commission requires setup code in Payload")
	}
	var res CommissionResult
	if err := a.client.Call(ctx, MethodCommission, CommissionParams{SetupCode: req.Payload}, &res); err != nil {
		return "", fmt.Errorf("matter: commission: %w", err)
	}
	if res.NodeID == "" {
		return "", errors.New("matter: sidecar returned empty nodeId")
	}
	a.log.Info("matter node commissioned", "nodeId", res.NodeID, "fabric", res.FabricIndex)
	return domain.TransportRef(res.NodeID), nil
}

// ReadState implements ports.Adapter.
func (a *Adapter) ReadState(ctx context.Context, ref domain.TransportRef, feature domain.FeatureKey, key domain.StateKey) (any, error) {
	binding, err := BindingFor(feature, key)
	if err != nil {
		return nil, err
	}
	params := AttrRef{
		NodeID:     string(ref),
		EndpointID: defaultEndpoint,
		Cluster:    binding.Cluster,
		Attribute:  binding.Attribute,
	}
	var raw json.RawMessage
	if err := a.client.Call(ctx, MethodReadAttribute, params, &raw); err != nil {
		return nil, fmt.Errorf("matter: readAttribute %s.%s: %w", binding.Cluster, binding.Attribute, err)
	}
	return decodeAttribute(feature, key, raw)
}

// WriteState implements ports.Adapter.
func (a *Adapter) WriteState(ctx context.Context, ref domain.TransportRef, feature domain.FeatureKey, key domain.StateKey, value any) error {
	binding, err := BindingFor(feature, key)
	if err != nil {
		return err
	}
	encoded, err := encodeAttribute(feature, key, value)
	if err != nil {
		return err
	}
	params := WriteAttrParams{
		AttrRef: AttrRef{
			NodeID:     string(ref),
			EndpointID: defaultEndpoint,
			Cluster:    binding.Cluster,
			Attribute:  binding.Attribute,
		},
		Value: encoded,
	}
	if err := a.client.Call(ctx, MethodWriteAttribute, params, nil); err != nil {
		return fmt.Errorf("matter: writeAttribute %s.%s: %w", binding.Cluster, binding.Attribute, err)
	}
	return nil
}

// InvokeAction implements ports.Adapter. Only the MVP subset is wired: OnOff
// On/Off/Toggle and LevelControl.MoveToLevel via ActionSet on Brightness.
func (a *Adapter) InvokeAction(ctx context.Context, ref domain.TransportRef, feature domain.FeatureKey, action domain.ActionKey, params map[string]any) error {
	invoke, err := actionToInvoke(ref, feature, action, params)
	if err != nil {
		return err
	}
	if err := a.client.Call(ctx, MethodInvokeCommand, invoke, nil); err != nil {
		return fmt.Errorf("matter: invoke %s.%s: %w", invoke.Cluster, invoke.Command, err)
	}
	return nil
}

// Subscribe implements ports.Adapter. Each caller gets an independent channel.
// The channel is closed when ctx is done or Stop is called.
func (a *Adapter) Subscribe(ctx context.Context) (<-chan ports.TransportEvent, error) {
	ch := make(chan ports.TransportEvent, 32)
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

// Decommission implements ports.Adapter.
func (a *Adapter) Decommission(ctx context.Context, ref domain.TransportRef) error {
	if err := a.client.Call(ctx, MethodRemoveNode, RemoveNodeParams{NodeID: string(ref)}, nil); err != nil {
		return fmt.Errorf("matter: removeNode %s: %w", ref, err)
	}
	return nil
}

// --- internals ---

// defaultEndpoint is the endpoint we address for single-function devices. Most
// lamps expose their functional clusters on endpoint 1. Multi-endpoint devices
// (a plug with two independently controlled sockets, a light strip with
// segments) will need per-Feature endpoint metadata — tracked as TODO for
// v1.1; see docs/matter-adapter-brief.md §5.
const defaultEndpoint = 1

func (a *Adapter) pumpEvents(ctx context.Context) {
	events := a.client.Events()
	for {
		select {
		case <-ctx.Done():
			return
		case ev, ok := <-events:
			if !ok {
				a.log.Warn("matter sidecar event stream closed")
				return
			}
			te, ok := transportEventFrom(ev, a.log)
			if !ok {
				continue
			}
			a.broadcast(te)
		}
	}
}

func (a *Adapter) broadcast(ev ports.TransportEvent) {
	a.subMu.RLock()
	defer a.subMu.RUnlock()
	for ch := range a.subs {
		select {
		case ch <- ev:
		default:
			a.log.Warn("matter subscriber slow, dropping event", "ref", ev.Ref, "kind", ev.Kind)
		}
	}
}

func (a *Adapter) closeAllSubs() {
	a.subMu.Lock()
	defer a.subMu.Unlock()
	for ch := range a.subs {
		close(ch)
		delete(a.subs, ch)
	}
}

// transportEventFrom decodes one sidecar Event into the ports.TransportEvent
// the ingress loop expects. Returns ok=false for events we don't yet handle
// (kept as info-level log; no failure).
func transportEventFrom(ev Event, log *slog.Logger) (ports.TransportEvent, bool) {
	switch ev.Name {
	case EventAttributeChanged:
		var a AttributeChanged
		if err := json.Unmarshal(ev.Data, &a); err != nil {
			log.Warn("matter: bad attributeChanged payload", "err", err)
			return ports.TransportEvent{}, false
		}
		feature, key, ok := FeatureForCluster(a.Cluster, a.Attribute)
		if !ok {
			return ports.TransportEvent{}, false
		}
		value, err := decodeAttributeAny(feature, key, a.Value)
		if err != nil {
			log.Warn("matter: decode attribute value", "cluster", a.Cluster, "attr", a.Attribute, "err", err)
			return ports.TransportEvent{}, false
		}
		return ports.TransportEvent{
			Ref:     domain.TransportRef(a.NodeID),
			Kind:    ports.TransportEventStateChanged,
			Feature: feature,
			Key:     string(key),
			Value:   value,
		}, true
	case EventNodeOnline:
		var n NodeLifecycle
		if err := json.Unmarshal(ev.Data, &n); err != nil {
			return ports.TransportEvent{}, false
		}
		return ports.TransportEvent{Ref: domain.TransportRef(n.NodeID), Kind: ports.TransportEventOnline}, true
	case EventNodeOffline:
		var n NodeLifecycle
		if err := json.Unmarshal(ev.Data, &n); err != nil {
			return ports.TransportEvent{}, false
		}
		return ports.TransportEvent{Ref: domain.TransportRef(n.NodeID), Kind: ports.TransportEventOffline}, true
	default:
		return ports.TransportEvent{}, false
	}
}
