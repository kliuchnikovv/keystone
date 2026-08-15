// Package service contains use-cases — orchestration between the domain,
// registry, event bus, and adapters. Handlers (HTTP/gRPC/WS) call into
// service; service knows nothing about the wire.
package service

import (
	"context"
	"errors"
	"fmt"
	"log/slog"
	"time"

	"github.com/kliuchnikovv/keystone/internal/domain"
	"github.com/kliuchnikovv/keystone/internal/ports"
	"github.com/kliuchnikovv/keystone/internal/registry"
)

// DeviceService coordinates commissioning, control, and lifecycle of devices
// across all registered adapters.
type DeviceService struct {
	log      *slog.Logger
	registry *registry.Registry
	bus      ports.EventBus
	adapters map[domain.TransportKind]ports.Adapter
}

// NewDeviceService builds the service. Adapters are keyed by their Kind() and
// must be Start()-ed by the caller before use.
func NewDeviceService(log *slog.Logger, reg *registry.Registry, bus ports.EventBus, adapters []ports.Adapter) *DeviceService {
	m := make(map[domain.TransportKind]ports.Adapter, len(adapters))
	for _, a := range adapters {
		m[a.Kind()] = a
	}
	return &DeviceService{
		log:      log,
		registry: reg,
		bus:      bus,
		adapters: m,
	}
}

// List returns every device currently in the registry.
func (s *DeviceService) List() []*domain.Device {
	return s.registry.List()
}

// AddDiscovered registers a device that was discovered from an adapter
// (as opposed to commissioned via user action). Used by transports that
// pair devices via their own app or expose a pre-populated device list.
func (s *DeviceService) AddDiscovered(d *domain.Device) error {
	if d.CreatedAt.IsZero() {
		d.CreatedAt = time.Now().UTC()
	}
	return s.registry.Add(d)
}

// Get returns one device by ID.
func (s *DeviceService) Get(id domain.DeviceID) (*domain.Device, error) {
	return s.registry.Get(id)
}

// Commission asks the specified transport to add a new device, then
// registers it locally.
func (s *DeviceService) Commission(ctx context.Context, transport domain.TransportKind, req ports.CommissionRequest, name string, deviceType domain.DeviceType) (*domain.Device, error) {
	adapter, ok := s.adapters[transport]
	if !ok {
		return nil, fmt.Errorf("no adapter registered for transport %q", transport)
	}

	ref, err := adapter.Commission(ctx, req)
	if err != nil {
		return nil, fmt.Errorf("commission: %w", err)
	}

	// After commissioning, ask the adapter to describe what it just added.
	ch, err := adapter.Discover(ctx)
	if err != nil {
		return nil, fmt.Errorf("discover after commission: %w", err)
	}
	var discovered *ports.DiscoveredDevice
	for d := range ch {
		if d.TransportRef == ref {
			cp := d
			discovered = &cp
			break
		}
	}
	if discovered == nil {
		return nil, errors.New("commissioned device not found in discovery")
	}

	// Materialise the domain.Device.
	d := &domain.Device{
		ID:           domain.DeviceID(domain.NewID()),
		Type:         deviceType,
		Name:         name,
		Manufacturer: discovered.Manufacturer,
		Model:        discovered.Model,
		Transport:    transport,
		TransportRef: ref,
		Features:     discovered.Features,
		Metadata:     discovered.Metadata,
		CreatedAt:    time.Now().UTC(),
	}
	if err := s.registry.Add(d); err != nil {
		return nil, err
	}
	s.log.Info("device commissioned", "id", d.ID, "type", d.Type, "name", d.Name)
	return d, nil
}

// Decommission removes a device: asks the adapter to unpair it from the
// transport, then evicts it from the registry. Errors from the adapter are
// returned to the caller but the registry entry is dropped regardless — a
// user asking to remove a device expects it to be gone from the UI even if
// the peer has already vanished from the network.
func (s *DeviceService) Decommission(ctx context.Context, id domain.DeviceID) error {
	release := s.registry.Lock(id)
	defer release()

	d, err := s.registry.Get(id)
	if err != nil {
		return err
	}
	adapter, ok := s.adapters[d.Transport]
	if !ok {
		return fmt.Errorf("no adapter registered for transport %q", d.Transport)
	}

	adapterErr := adapter.Decommission(ctx, d.TransportRef)
	if removeErr := s.registry.Remove(id); removeErr != nil && adapterErr == nil {
		return removeErr
	}
	s.log.Info("device decommissioned", "id", id, "transport", d.Transport, "adapter_err", adapterErr)
	return adapterErr
}

// DiscoverCommissionable asks one transport to scan for devices that are not
// yet commissioned. Returns an error if the transport is unknown or has no
// concept of commissionable discovery (most don't).
//
// Finding a device does not mean it can be added: pairing still needs the setup
// code from the device itself, which is never advertised.
func (s *DeviceService) DiscoverCommissionable(
	ctx context.Context,
	transport domain.TransportKind,
	window time.Duration,
) (<-chan ports.CommissionableDevice, error) {
	adapter, ok := s.adapters[transport]
	if !ok {
		return nil, fmt.Errorf("no adapter registered for transport %q", transport)
	}
	scanner, ok := adapter.(ports.CommissionableDiscoverer)
	if !ok {
		return nil, fmt.Errorf("transport %q does not support scanning for commissionable devices", transport)
	}
	return scanner.DiscoverCommissionable(ctx, window)
}

// CameraStreamer returns the transport behind a device if it can broker a live
// video session. The device id is resolved here so callers never have to know
// which transport a camera is on.
func (s *DeviceService) CameraStreamer(id domain.DeviceID) (ports.CameraStreamer, *domain.Device, error) {
	d, err := s.registry.Get(id)
	if err != nil {
		return nil, nil, err
	}
	adapter, ok := s.adapters[d.Transport]
	if !ok {
		return nil, nil, fmt.Errorf("no adapter registered for transport %q", d.Transport)
	}
	streamer, ok := adapter.(ports.CameraStreamer)
	if !ok {
		return nil, nil, fmt.Errorf("transport %q cannot stream video", d.Transport)
	}
	if !d.HasFeature(domain.FeatureCamera) {
		return nil, nil, fmt.Errorf("device %s is not a camera", id)
	}
	return streamer, d, nil
}

// SyncFromAdapters walks every registered adapter, calls Discover, and
// registers any peer the adapter reports but the registry does not know
// about yet. Intended to run once at boot so a matter.js sidecar with
// pre-commissioned devices (typical case: keystone restarted, fabric on
// disk survived) rehydrates the domain registry automatically. Returns the
// list of newly added devices so callers can persist them.
func (s *DeviceService) SyncFromAdapters(ctx context.Context) (added, updated []*domain.Device, err error) {
	// Index existing devices by transport-ref so we can distinguish
	// "new to keystone" (add) from "already known, features maybe stale"
	// (refresh) — the latter happens after a sidecar upgrade that starts
	// reporting cluster info we couldn't see before.
	knownByKey := make(map[string]*domain.Device)
	for _, d := range s.registry.List() {
		knownByKey[string(d.Transport)+":"+string(d.TransportRef)] = d
	}

	var firstErr error
	for kind, adapter := range s.adapters {
		ch, err := adapter.Discover(ctx)
		if err != nil {
			s.log.Warn("adapter discover failed", "transport", kind, "err", err)
			if firstErr == nil {
				firstErr = err
			}
			continue
		}
		for disc := range ch {
			key := string(kind) + ":" + string(disc.TransportRef)
			if existing, ok := knownByKey[key]; ok {
				if s.registry.UpdateDiscoveryInfo(existing.ID, disc.Type, disc.Manufacturer, disc.Model, disc.Features) {
					s.log.Info("device features refreshed from adapter",
						"id", existing.ID, "transport", kind, "features", len(disc.Features))
					// Re-fetch the updated snapshot so caller can persist it.
					if refreshed, err := s.registry.Get(existing.ID); err == nil {
						updated = append(updated, refreshed)
					}
				}
				continue
			}
			d := &domain.Device{
				ID:           domain.DeviceID(domain.NewID()),
				Type:         disc.Type,
				Name:         disc.Name,
				Manufacturer: disc.Manufacturer,
				Model:        disc.Model,
				Transport:    kind,
				TransportRef: disc.TransportRef,
				Features:     disc.Features,
				Metadata:     disc.Metadata,
				CreatedAt:    time.Now().UTC(),
			}
			if err := s.registry.Add(d); err != nil {
				s.log.Warn("sync: registry add failed", "transport", kind, "ref", disc.TransportRef, "err", err)
				continue
			}
			s.log.Info("device synced from adapter", "id", d.ID, "transport", kind, "name", d.Name)
			knownByKey[key] = d
			added = append(added, d)
		}
	}
	return added, updated, firstErr
}

// InvokeAction executes an action on a device, holding the per-device lock
// so concurrent writers to the same device serialise.
func (s *DeviceService) InvokeAction(ctx context.Context, id domain.DeviceID, feature domain.FeatureKey, action domain.ActionKey, params map[string]any) error {
	release := s.registry.Lock(id)
	defer release()

	d, err := s.registry.Get(id)
	if err != nil {
		return err
	}
	adapter, ok := s.adapters[d.Transport]
	if !ok {
		return fmt.Errorf("no adapter registered for transport %q", d.Transport)
	}

	return adapter.InvokeAction(ctx, d.TransportRef, feature, action, params)
}

// WriteState sets an attribute of a device, again serialising per-device.
func (s *DeviceService) WriteState(ctx context.Context, id domain.DeviceID, feature domain.FeatureKey, key domain.StateKey, value any) error {
	release := s.registry.Lock(id)
	defer release()

	d, err := s.registry.Get(id)
	if err != nil {
		return err
	}
	adapter, ok := s.adapters[d.Transport]
	if !ok {
		return fmt.Errorf("no adapter registered for transport %q", d.Transport)
	}

	return adapter.WriteState(ctx, d.TransportRef, feature, key, value)
}

// ReadState reads an attribute from a device (no lock — reads are cheap).
func (s *DeviceService) ReadState(ctx context.Context, id domain.DeviceID, feature domain.FeatureKey, key domain.StateKey) (any, error) {
	d, err := s.registry.Get(id)
	if err != nil {
		return nil, err
	}
	adapter, ok := s.adapters[d.Transport]
	if !ok {
		return nil, fmt.Errorf("no adapter registered for transport %q", d.Transport)
	}
	return adapter.ReadState(ctx, d.TransportRef, feature, key)
}

// IngressLoop consumes TransportEvents from every adapter and publishes them
// onto the event bus after mapping to StateSnapshot / Event.
//
// Call this once per adapter at startup, in a goroutine. It exits when ctx
// is cancelled.
func (s *DeviceService) IngressLoop(ctx context.Context, adapter ports.Adapter) error {
	ch, err := adapter.Subscribe(ctx)
	if err != nil {
		return fmt.Errorf("subscribe to %s: %w", adapter.Kind(), err)
	}

	// Build a reverse index TransportRef -> DeviceID for this transport.
	// A production version would keep this in the registry; for POC we scan
	// on every event which is fine at small N.
	findDeviceByRef := func(ref domain.TransportRef) (*domain.Device, bool) {
		for _, d := range s.registry.List() {
			if d.Transport == adapter.Kind() && d.TransportRef == ref {
				return d, true
			}
		}
		return nil, false
	}

	for {
		select {
		case <-ctx.Done():
			return ctx.Err()
		case ev, ok := <-ch:
			if !ok {
				return nil
			}
			// Adapter-level status carries no device ref, so it must be
			// handled before the per-device lookup below.
			if ev.Kind == ports.TransportEventAdapterStatus {
				connected, _ := ev.Value.(bool)
				if connected {
					s.log.Info("adapter backend reachable", "transport", adapter.Kind())
				} else {
					s.log.Warn("adapter backend unreachable — device state may be stale",
						"transport", adapter.Kind())
				}
				continue
			}

			d, found := findDeviceByRef(ev.Ref)
			if !found {
				s.log.Debug("transport event for unknown device",
					"transport", adapter.Kind(), "ref", ev.Ref)
				continue
			}
			switch ev.Kind {
			case ports.TransportEventStateChanged:
				snap := domain.StateSnapshot{
					DeviceID:  d.ID,
					Feature:   ev.Feature,
					Key:       domain.StateKey(ev.Key),
					Value:     ev.Value,
					UpdatedAt: time.Now().UTC(),
					Origin:    domain.OriginDeviceReport,
				}
				s.registry.PutState(snap)
				_ = s.bus.PublishState(ctx, snap)
			case ports.TransportEventFired:
				data, _ := ev.Value.(map[string]any)
				_ = s.bus.PublishEvent(ctx, domain.Event{
					DeviceID: d.ID,
					Feature:  ev.Feature,
					Name:     domain.EventKey(ev.Key),
					Data:     data,
					At:       time.Now().UTC(),
				})
			}
		}
	}
}
