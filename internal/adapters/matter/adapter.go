package matter

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"log/slog"
	"sync"
	"sync/atomic"
	"time"

	"github.com/kliuchnikovv/keystone/internal/domain"
	"github.com/kliuchnikovv/keystone/internal/ports"
)

// Adapter implements ports.Adapter for Matter devices via a matter.js
// sidecar (WebSocket JSON-RPC). See docs/matter-adapter-brief.md.
//
// The adapter always registers itself with the core, even if the sidecar
// is unreachable at boot: a background goroutine retries the WebSocket
// connect with exponential backoff, and every ports.Adapter method returns
// ErrSidecarUnavailable while disconnected. This avoids the "no adapter
// registered for transport matter" failure mode when keystone races the
// sidecar container on Docker startup.
type Adapter struct {
	log    *slog.Logger
	cfg    Config
	client Client

	mu      sync.Mutex
	started bool

	// connected reflects the current sidecar reachability. Every RPC method
	// checks it before dispatching and returns ErrSidecarUnavailable if false.
	connected atomic.Bool

	// Fan-out of TransportEvent to any Subscribe() callers. matter.js only
	// pushes on one WebSocket, so we own a single ingress goroutine and
	// broadcast to N subscribers.
	subMu sync.RWMutex
	subs  map[chan ports.TransportEvent]struct{}

	// progressMu guards the sink that receives commissioningProgress events
	// while a Commission call is in flight. The sidecar allows one
	// commissioning session at a time, so a single sink is enough.
	progressMu sync.Mutex
	progress   func(stage, message string)

	// foundMu guards the sink that receives commissionableFound events while a
	// scan is running. One scan at a time, like commissioning.
	foundMu   sync.Mutex
	found     chan<- ports.CommissionableDevice
	foundSeen map[string]struct{}

	// signalsMu guards the set of viewers waiting on camera WebRTC signalling.
	signalsMu sync.RWMutex
	signals   map[chan ports.CameraSignal]struct{}

	// lastSeq is the highest sidecar event sequence we have processed. Sent
	// back on resubscribe so the sidecar can replay exactly what we missed.
	lastSeq atomic.Int64

	// routesMu guards routes: per-node, which endpoint each Feature lives on.
	// Built during Discover and refreshed on demand; see endpointFor.
	routesMu sync.RWMutex
	routes   map[domain.TransportRef]nodeRoutes

	// cancelIngress stops the goroutines that pump client.Events() into
	// subscribers and retry connections. Set on Start, called on Stop.
	cancelIngress context.CancelFunc
}

// ErrSidecarUnavailable is returned by any RPC method when the sidecar
// WebSocket is not currently connected. The adapter is still registered and
// will attempt to reconnect in the background.
var ErrSidecarUnavailable = errors.New("matter sidecar not connected yet — the reconnect loop is retrying in the background")

// New builds a Matter adapter. The Client is injected so tests can supply
// a mock and so the WebSocket implementation can evolve independently.
func New(log *slog.Logger, cfg Config, client Client) *Adapter {
	if log == nil {
		log = slog.Default()
	}
	return &Adapter{
		log:     log,
		cfg:     cfg,
		client:  client,
		subs:    make(map[chan ports.TransportEvent]struct{}),
		routes:  make(map[domain.TransportRef]nodeRoutes),
		signals: make(map[chan ports.CameraSignal]struct{}),
	}
}

// Kind implements ports.Adapter.
func (a *Adapter) Kind() domain.TransportKind { return domain.TransportMatter }

// Start implements ports.Adapter. It tries to dial the sidecar once
// synchronously — if that succeeds, the event pump starts immediately. If
// the sidecar isn't ready yet (common on Docker startup where keystone
// races the sidecar container), Start still returns nil and a background
// goroutine retries with exponential backoff. This keeps the adapter
// registered so RPC calls get a clear "not connected" error instead of the
// misleading "no adapter registered for transport matter".
func (a *Adapter) Start(ctx context.Context) error {
	a.mu.Lock()
	defer a.mu.Unlock()
	if a.started {
		return errors.New("matter adapter already started")
	}
	if a.client == nil {
		return errors.New("matter adapter: nil client")
	}

	pumpCtx, cancel := context.WithCancel(context.Background())
	a.cancelIngress = cancel
	a.started = true

	// Try once synchronously so happy-path startup logs "connected" right away.
	if err := a.client.Connect(ctx); err == nil {
		a.setConnected(true)
		a.resubscribe(ctx)
		go a.pumpEvents(pumpCtx)
		a.log.Info("matter adapter started", "sidecar", a.cfg.URL())
		return nil
	} else {
		a.log.Warn("matter sidecar unreachable at boot — will retry in background",
			"sidecar", a.cfg.URL(), "err", err)
		go a.connectLoop(pumpCtx)
		return nil
	}
}

// connectLoop retries client.Connect with exponential backoff until it
// succeeds or the adapter is stopped. Once connected, it starts the event
// pump and exits.
func (a *Adapter) connectLoop(ctx context.Context) {
	backoff := 1 * time.Second
	const maxBackoff = 30 * time.Second

	for {
		select {
		case <-ctx.Done():
			return
		case <-time.After(backoff):
		}

		dialCtx, cancel := context.WithTimeout(ctx, 5*time.Second)
		err := a.client.Connect(dialCtx)
		cancel()

		if err == nil {
			a.setConnected(true)
			a.log.Info("matter adapter connected after retry", "sidecar", a.cfg.URL())
			a.resubscribe(ctx)
			go a.pumpEvents(ctx)
			return
		}

		a.log.Debug("matter sidecar reconnect attempt failed", "err", err, "next_in", backoff.String())
		backoff *= 2
		if backoff > maxBackoff {
			backoff = maxBackoff
		}
	}
}

// Stop implements ports.Adapter.
func (a *Adapter) Stop(ctx context.Context) error {
	a.mu.Lock()
	defer a.mu.Unlock()
	if !a.started {
		return nil
	}
	a.started = false
	a.setConnected(false)
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
	if !a.connected.Load() {
		return nil, ErrSidecarUnavailable
	}
	var nodes []Node
	if err := a.client.Call(ctx, MethodListNodes, nil, &nodes); err != nil {
		return nil, fmt.Errorf("matter: listNodes: %w", err)
	}
	out := make(chan ports.DiscoveredDevice, len(nodes))
	for _, n := range nodes {
		disc, routes := nodeToDiscovered(n)
		a.storeRoutes(disc.TransportRef, nodeRoutes{features: routes, clusters: clustersFromNode(n)})
		out <- disc
	}
	close(out)
	return out, nil
}

func (a *Adapter) storeRoutes(ref domain.TransportRef, routes nodeRoutes) {
	if len(routes.features) == 0 {
		return
	}
	a.routesMu.Lock()
	a.routes[ref] = routes
	a.routesMu.Unlock()
}

// clustersOn reports the clusters the node exposes on one endpoint. Empty when
// the layout is unknown, which BindingFor treats as "use the first candidate".
func (a *Adapter) clustersOn(ref domain.TransportRef, endpoint int) []string {
	a.routesMu.RLock()
	defer a.routesMu.RUnlock()
	return a.routes[ref].clusters[endpoint]
}

// bindingFor resolves the Matter address for a (feature, state) pair on the
// endpoint that feature actually lives on.
func (a *Adapter) bindingFor(ctx context.Context, ref domain.TransportRef, feature domain.FeatureKey, key domain.StateKey) (FeatureBinding, int, error) {
	endpoint := a.endpointFor(ctx, ref, feature)
	binding, err := BindingFor(feature, key, a.clustersOn(ref, endpoint))
	if err != nil {
		return FeatureBinding{}, endpoint, err
	}
	return binding, endpoint, nil
}

// endpointFor resolves which endpoint a feature lives on for a given node.
//
// Routes are learned from listNodes (Discover at boot, and again after each
// commission). On a miss — a device commissioned by another keystone instance,
// or a call that raced the first sync — one listNodes refresh is attempted
// before falling back to defaultEndpoint, which is what the adapter used
// unconditionally before per-feature routing existed.
func (a *Adapter) endpointFor(ctx context.Context, ref domain.TransportRef, feature domain.FeatureKey) int {
	if ep, ok := a.lookupRoute(ref, feature); ok {
		return ep
	}
	if err := a.refreshRoutes(ctx); err != nil {
		a.log.Debug("matter: route refresh failed", "ref", ref, "err", err)
	}
	if ep, ok := a.lookupRoute(ref, feature); ok {
		return ep
	}
	a.log.Debug("matter: no endpoint route, using default",
		"ref", ref, "feature", feature, "endpoint", defaultEndpoint)
	return defaultEndpoint
}

func (a *Adapter) lookupRoute(ref domain.TransportRef, feature domain.FeatureKey) (int, bool) {
	a.routesMu.RLock()
	defer a.routesMu.RUnlock()
	ep, ok := a.routes[ref].features[feature]
	return ep, ok
}

// refreshRoutes re-reads the whole fabric's endpoint layout. Cheap enough to
// run on a routing miss: listNodes is served from the sidecar's local cache.
func (a *Adapter) refreshRoutes(ctx context.Context) error {
	var nodes []Node
	if err := a.client.Call(ctx, MethodListNodes, nil, &nodes); err != nil {
		return err
	}
	for _, n := range nodes {
		_, routes := featuresFromNode(n)
		a.storeRoutes(domain.TransportRef(n.NodeID), nodeRoutes{features: routes, clusters: clustersFromNode(n)})
	}
	return nil
}

// Commission implements ports.Adapter. For Matter via multi-admin from an
// existing ecosystem, req.Payload carries the 11-digit setup code the user
// copied from Home.app / Google Home / SmartThings' "Turn on Pairing Mode"
// screen.
func (a *Adapter) Commission(ctx context.Context, req ports.CommissionRequest) (domain.TransportRef, error) {
	if !a.connected.Load() {
		return "", ErrSidecarUnavailable
	}
	if req.Payload == "" {
		return "", errors.New("matter: commission requires setup code in Payload")
	}
	if req.Progress != nil {
		a.progressMu.Lock()
		a.progress = req.Progress
		a.progressMu.Unlock()
		defer func() {
			a.progressMu.Lock()
			a.progress = nil
			a.progressMu.Unlock()
		}()
	}

	var res CommissionResult
	params := CommissionParams{SetupCode: req.Payload, Target: req.Extra["matter.target"]}
	// Wi-Fi credentials were being dropped here: the port advertised the
	// fields and nothing carried them, so a device that still had to join a
	// network could never be commissioned.
	if req.WifiSSID != "" {
		params.Network = &NetworkCredentials{
			Wifi: &WifiCredentials{SSID: req.WifiSSID, Credentials: req.WifiCred},
		}
	}
	// Thread needs the operational dataset, which only the border router can
	// hand out; it travels through Extra until keystone has a way to obtain it
	// on its own.
	if dataset := req.Extra["matter.threadDataset"]; dataset != "" {
		if params.Network == nil {
			params.Network = &NetworkCredentials{}
		}
		params.Network.Thread = &ThreadCredentials{
			OperationalDataset: dataset,
			NetworkName:        req.Extra["matter.threadNetwork"],
		}
	}
	if err := a.client.Call(ctx, MethodCommission, params, &res); err != nil {
		return "", fmt.Errorf("matter: commission: %w", err)
	}
	if res.NodeID == "" {
		return "", errors.New("matter: sidecar returned empty nodeId")
	}
	a.log.Info("matter node commissioned", "nodeId", res.NodeID, "fabric", res.FabricIndex)
	// Learn the new node's endpoint layout now, so the first command the user
	// sends doesn't have to fall back to defaultEndpoint.
	if err := a.refreshRoutes(ctx); err != nil {
		a.log.Debug("matter: route refresh after commission failed", "nodeId", res.NodeID, "err", err)
	}
	return domain.TransportRef(res.NodeID), nil
}

// ReadState implements ports.Adapter.
func (a *Adapter) ReadState(ctx context.Context, ref domain.TransportRef, feature domain.FeatureKey, key domain.StateKey) (any, error) {
	if !a.connected.Load() {
		return nil, ErrSidecarUnavailable
	}
	binding, endpoint, err := a.bindingFor(ctx, ref, feature, key)
	if err != nil {
		return nil, err
	}
	params := AttrRef{
		NodeID:     string(ref),
		EndpointID: endpoint,
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
	if !a.connected.Load() {
		return ErrSidecarUnavailable
	}
	// Some states are read-only in Matter and change only through a command —
	// brightness and colour among them. Writing the attribute is silently
	// ineffective on real hardware, so translate instead of pretending.
	if param, viaCommand := WriteAsCommand(feature, key); viaCommand {
		endpoint := a.endpointFor(ctx, ref, feature)
		invoke, err := actionToInvoke(ref, endpoint, a.clustersOn(ref, endpoint),
			feature, domain.ActionSet, map[string]any{param: value})
		if err != nil {
			return err
		}
		if err := a.client.Call(ctx, MethodInvokeCommand, invoke, nil); err != nil {
			return fmt.Errorf("matter: set %s.%s via %s.%s: %w",
				feature, key, invoke.Cluster, invoke.Command, err)
		}
		return nil
	}

	binding, endpoint, err := a.bindingFor(ctx, ref, feature, key)
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
			EndpointID: endpoint,
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
	if !a.connected.Load() {
		return ErrSidecarUnavailable
	}
	endpoint := a.endpointFor(ctx, ref, feature)
	invoke, err := actionToInvoke(ref, endpoint, a.clustersOn(ref, endpoint), feature, action, params)
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
//
// Note: Subscribe is allowed even while the sidecar is disconnected — the
// subscriber will simply receive no events until the reconnect loop lands
// a connection. This lets the UI attach to the stream at any time.
func (a *Adapter) Subscribe(ctx context.Context) (<-chan ports.TransportEvent, error) {
	ch := make(chan ports.TransportEvent, 32)
	a.subMu.Lock()
	a.subs[ch] = struct{}{}
	a.subMu.Unlock()

	// Seed the current status so a subscriber knows straight away whether the
	// silence it is about to hear means "nothing happening" or "sidecar down".
	ch <- adapterStatusEvent(a.connected.Load())

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
	if !a.connected.Load() {
		return ErrSidecarUnavailable
	}
	var res RemoveNodeResult
	if err := a.client.Call(ctx, MethodRemoveNode, RemoveNodeParams{NodeID: string(ref)}, &res); err != nil {
		return fmt.Errorf("matter: removeNode %s: %w", ref, err)
	}
	a.forgetNode(ref)
	if res.Removed == "forced" {
		// Surfaced as an error so the delete handler reports it: the device is
		// gone from keystone, but it still lists us as a connected service and
		// the user is the only one who can fix that.
		a.log.Warn("matter device removed without decommissioning", "ref", ref, "detail", res.Message)
		return ErrForcedRemoval
	}
	a.log.Info("matter device decommissioned", "ref", ref)
	return nil
}

// forgetNode drops cached routing for a device that is no longer ours, so a
// later device reusing the same ref cannot inherit stale endpoints.
func (a *Adapter) forgetNode(ref domain.TransportRef) {
	a.routesMu.Lock()
	delete(a.routes, ref)
	a.routesMu.Unlock()
}

// --- internals ---

// defaultEndpoint is the endpoint we address for single-function devices. Most
// lamps expose their functional clusters on endpoint 1. Multi-endpoint devices
// (a plug with two independently controlled sockets, a light strip with
// segments) will need per-Feature endpoint metadata — tracked as TODO for
// v1.1; see docs/matter-adapter-brief.md §5.
const defaultEndpoint = 1

// pumpEvents forwards sidecar events to subscribers until the adapter stops or
// the connection drops. On a drop it hands back to connectLoop — without that
// hand-off a sidecar restart mid-day left the adapter permanently deaf while
// still reporting itself as started.
func (a *Adapter) pumpEvents(ctx context.Context) {
	events := a.client.Events()
	disconnected := a.client.Disconnected()
	for {
		select {
		case <-ctx.Done():
			return
		case <-disconnected:
			a.setConnected(false)
			a.log.Warn("matter sidecar connection lost — reconnecting", "sidecar", a.cfg.URL())
			go a.connectLoop(ctx)
			return
		case ev, ok := <-events:
			if !ok {
				a.log.Warn("matter sidecar event stream closed")
				a.setConnected(false)
				return
			}
			if !a.acceptSeq(ev.Seq) {
				continue
			}
			// Commissioning progress is scoped to an in-flight Commission call,
			// not to a device — it arrives before the device has an identity.
			if ev.Name == EventCommissioningStage {
				a.reportProgress(ev)
				continue
			}
			if ev.Name == EventCommissionableFound {
				a.reportCommissionable(ev)
				continue
			}
			if ev.Name == EventWebrtcSignal {
				a.broadcastSignal(ev)
				continue
			}
			te, ok := transportEventFrom(ev, a.log)
			if !ok {
				continue
			}
			a.broadcast(te)
		}
	}
}

// resubscribe tells the sidecar where we left off. It replays the events we
// missed during the outage, or publishes a full snapshot when the gap is too
// large to replay — either way the adapter is back in sync before the event
// pump starts, instead of silently carrying stale state until something moves.
func (a *Adapter) resubscribe(ctx context.Context) {
	callCtx, cancel := context.WithTimeout(ctx, 10*time.Second)
	defer cancel()

	var res SubscribeResult
	if err := a.client.Call(callCtx, MethodSubscribe, SubscribeParams{SinceSeq: a.lastSeq.Load()}, &res); err != nil {
		// An older sidecar has no subscribe method. Nothing to do: it also
		// sends no seq, so acceptSeq lets everything through.
		a.log.Debug("matter: resubscribe failed", "err", err, "kind", KindOf(err))
		return
	}

	// Clamp: if the sidecar's position is behind ours, its process restarted
	// and renumbered from 1. Keeping our old counter would make every fresh
	// event look like a duplicate and mute the adapter for good.
	if res.Seq < a.lastSeq.Load() {
		a.log.Info("matter sidecar restarted — resetting event sequence",
			"ours", a.lastSeq.Load(), "theirs", res.Seq)
		a.lastSeq.Store(res.Seq)
	}
	if res.Gap {
		a.lastSeq.Store(res.Seq)
	}

	a.log.Info("matter adapter resubscribed",
		"since", a.lastSeq.Load(), "replayed", res.Replayed, "snapshot", res.Gap)
}

// reportProgress forwards one commissioningProgress event to whoever is
// currently commissioning. Dropped when nobody is: a stray progress event after
// the call returned has no one to tell.
func (a *Adapter) reportProgress(ev Event) {
	a.progressMu.Lock()
	sink := a.progress
	a.progressMu.Unlock()
	if sink == nil {
		return
	}
	var p CommissioningProgress
	if err := json.Unmarshal(ev.Data, &p); err != nil {
		a.log.Warn("matter: bad commissioningProgress payload", "err", err)
		return
	}
	sink(p.Stage, p.Message)
}

// DiscoverCommissionable implements ports.CommissionableDiscoverer. It runs one
// scan on the sidecar and streams advertisements as they arrive, so a UI can
// fill its list during the scan rather than after it.
//
// The returned channel is closed when the scan ends. Only one scan runs at a
// time; a second call while one is in flight is refused.
func (a *Adapter) DiscoverCommissionable(ctx context.Context, window time.Duration) (<-chan ports.CommissionableDevice, error) {
	if !a.connected.Load() {
		return nil, ErrSidecarUnavailable
	}
	if window <= 0 {
		window = 10 * time.Second
	}

	out := make(chan ports.CommissionableDevice, 16)
	// Refs already delivered live, so the reconciliation below does not repeat
	// them. Guarded by foundMu together with the sink itself.
	seen := make(map[string]struct{})

	a.foundMu.Lock()
	if a.found != nil {
		a.foundMu.Unlock()
		return nil, errors.New("matter: a commissionable scan is already running")
	}
	a.found = out
	a.foundSeen = seen
	a.foundMu.Unlock()

	go func() {
		defer func() {
			a.foundMu.Lock()
			a.found = nil
			a.foundSeen = nil
			a.foundMu.Unlock()
			close(out)
		}()

		// Allow for the round trip on top of the scan window itself.
		callCtx, cancel := context.WithTimeout(ctx, window+15*time.Second)
		defer cancel()

		var devices []CommissionableDevice
		params := DiscoverCommissionableParams{TimeoutMs: window.Milliseconds()}
		if err := a.client.Call(callCtx, MethodDiscoverCommissionable, params, &devices); err != nil {
			a.log.Warn("matter: commissionable scan failed", "err", err, "kind", KindOf(err))
			return
		}
		// Reconcile against the scan's own result. A device the sidecar found but
		// whose live event went missing would otherwise never reach the caller —
		// the list would look empty while the log said "found 1", which is
		// exactly the confusing combination this guards against.
		a.foundMu.Lock()
		var missed int
		for _, d := range devices {
			if _, ok := seen[d.Ref]; ok {
				continue
			}
			seen[d.Ref] = struct{}{}
			missed++
			select {
			case out <- commissionableFrom(d):
			default:
			}
		}
		a.foundMu.Unlock()
		if missed > 0 {
			a.log.Warn("matter: commissionable devices arrived only in the scan result",
				"count", missed, "of", len(devices))
		}

		a.log.Info("matter commissionable scan finished", "window", window.String(), "found", len(devices))
	}()

	return out, nil
}

// reportCommissionable forwards one commissionableFound event to the scan in
// progress. Events arriving outside a scan are dropped.
func (a *Adapter) reportCommissionable(ev Event) {
	a.foundMu.Lock()
	sink := a.found
	a.foundMu.Unlock()
	if sink == nil {
		return
	}
	var d CommissionableDevice
	if err := json.Unmarshal(ev.Data, &d); err != nil {
		a.log.Warn("matter: bad commissionableFound payload", "err", err)
		return
	}
	a.foundMu.Lock()
	if a.foundSeen != nil {
		if _, dup := a.foundSeen[d.Ref]; dup {
			a.foundMu.Unlock()
			return
		}
		a.foundSeen[d.Ref] = struct{}{}
	}
	a.foundMu.Unlock()
	a.log.Info("matter commissionable device found", "ref", d.Ref, "name", d.Name,
		"deviceType", d.DeviceType, "discriminator", d.Discriminator)
	select {
	case sink <- commissionableFrom(d):
	default:
		a.log.Warn("matter: commissionable consumer slow, dropping", "ref", d.Ref)
	}
}

// acceptSeq reports whether an event should be processed, recording it as the
// new high-water mark. Replay after a reconnect can legitimately resend events
// we already handled — dropping them here keeps the rest of the pipeline free
// of duplicate-handling logic.
func (a *Adapter) acceptSeq(seq int64) bool {
	if seq == 0 {
		return true // sidecar doesn't sequence events
	}
	for {
		last := a.lastSeq.Load()
		if seq <= last {
			return false
		}
		if a.lastSeq.CompareAndSwap(last, seq) {
			return true
		}
	}
}

// setConnected records reachability and tells subscribers about a change.
// Only transitions are broadcast — connectLoop and pumpEvents can both report
// the same state, and repeating it would be noise.
func (a *Adapter) setConnected(connected bool) {
	if a.connected.Swap(connected) == connected {
		return
	}
	a.broadcast(adapterStatusEvent(connected))
}

func adapterStatusEvent(connected bool) ports.TransportEvent {
	return ports.TransportEvent{Kind: ports.TransportEventAdapterStatus, Value: connected}
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
	case EventDeviceEvent:
		var d DeviceEvent
		if err := json.Unmarshal(ev.Data, &d); err != nil {
			log.Warn("matter: bad deviceEvent payload", "err", err)
			return ports.TransportEvent{}, false
		}
		feature, key, ok := EventForCluster(d.Cluster, d.Event)
		if !ok {
			return ports.TransportEvent{}, false
		}
		return ports.TransportEvent{
			Ref:     domain.TransportRef(d.NodeID),
			Kind:    ports.TransportEventFired,
			Feature: feature,
			Key:     string(key),
			Value:   d.Data,
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

// --- camera streaming (ports.CameraStreamer) ---

// StartStream implements ports.CameraStreamer. The SDP comes from the viewer's
// browser; keystone only relays it. Nothing here touches media.
func (a *Adapter) StartStream(ctx context.Context, ref domain.TransportRef, sdp string) (int, error) {
	if !a.connected.Load() {
		return 0, ErrSidecarUnavailable
	}
	endpoint := a.endpointFor(ctx, ref, domain.FeatureCamera)
	var res WebrtcOfferResult
	params := WebrtcOfferParams{NodeID: string(ref), EndpointID: endpoint, SDP: sdp}
	if err := a.client.Call(ctx, MethodWebrtcOffer, params, &res); err != nil {
		return 0, fmt.Errorf("matter: webrtc offer: %w", err)
	}
	a.log.Info("camera stream opened", "ref", ref, "session", res.SessionID)
	return res.SessionID, nil
}

// AddCandidates implements ports.CameraStreamer.
func (a *Adapter) AddCandidates(ctx context.Context, sessionID int, candidates []string) error {
	if !a.connected.Load() {
		return ErrSidecarUnavailable
	}
	if len(candidates) == 0 {
		return nil
	}
	params := WebrtcIceParams{SessionID: sessionID, Candidates: candidates}
	if err := a.client.Call(ctx, MethodWebrtcIce, params, nil); err != nil {
		return fmt.Errorf("matter: webrtc ice: %w", err)
	}
	return nil
}

// StopStream implements ports.CameraStreamer.
func (a *Adapter) StopStream(ctx context.Context, sessionID int) error {
	if !a.connected.Load() {
		return ErrSidecarUnavailable
	}
	if err := a.client.Call(ctx, MethodWebrtcStop, WebrtcStopParams{SessionID: sessionID}, nil); err != nil {
		return fmt.Errorf("matter: webrtc stop: %w", err)
	}
	return nil
}

// Signals implements ports.CameraStreamer. Every subscriber sees every signal;
// callers filter by session id, which is cheap and avoids a registry that would
// have to be cleaned up when a viewer disappears mid-handshake.
func (a *Adapter) Signals(ctx context.Context) (<-chan ports.CameraSignal, error) {
	ch := make(chan ports.CameraSignal, 16)

	a.signalsMu.Lock()
	a.signals[ch] = struct{}{}
	a.signalsMu.Unlock()

	go func() {
		<-ctx.Done()
		a.signalsMu.Lock()
		if _, ok := a.signals[ch]; ok {
			delete(a.signals, ch)
			close(ch)
		}
		a.signalsMu.Unlock()
	}()
	return ch, nil
}

// broadcastSignal fans one camera signal out to every viewer.
func (a *Adapter) broadcastSignal(ev Event) {
	var sig WebrtcSignal
	if err := json.Unmarshal(ev.Data, &sig); err != nil {
		a.log.Warn("matter: bad webrtcSignal payload", "err", err)
		return
	}
	out := ports.CameraSignal{
		Kind:       sig.Kind,
		SessionID:  sig.SessionID,
		SDP:        sig.SDP,
		Candidates: sig.Candidates,
		Reason:     sig.Reason,
	}
	a.signalsMu.RLock()
	defer a.signalsMu.RUnlock()
	for ch := range a.signals {
		select {
		case ch <- out:
		default:
			// Dropping a signalling message breaks the handshake, so this is a
			// warning rather than a debug line.
			a.log.Warn("camera signal dropped, viewer too slow", "session", sig.SessionID, "kind", sig.Kind)
		}
	}
}
