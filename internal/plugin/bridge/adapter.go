package bridge

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"log/slog"

	"github.com/kliuchnikovv/keystone-api/sidecar"

	"github.com/kliuchnikovv/keystone/internal/domain"
	"github.com/kliuchnikovv/keystone/internal/ports"
)

// Adapter is a ports.Adapter backed by a sidecar plugin.
//
// A plugin is talked to over one *sidecar.Client. The client is owned by
// the plugin manager and stays live across plugin restarts, so this
// Adapter is created once when the plugin becomes Running and torn down
// when it goes back to Stopped.
type Adapter struct {
	client *sidecar.Client
	kind   domain.TransportKind
	log    *slog.Logger
}

// New builds an Adapter with a live sidecar client. kind is the transport
// identifier the plugin declared — the manager reads it from the manifest
// (spec.capabilities: transport.<kind>) and hands it in here.
func New(client *sidecar.Client, kind domain.TransportKind, log *slog.Logger) *Adapter {
	if log == nil {
		log = slog.Default()
	}
	return &Adapter{client: client, kind: kind, log: log}
}

// Kind implements ports.Adapter.
func (a *Adapter) Kind() domain.TransportKind { return a.kind }

// Start subscribes to the adapter event topic and pings the plugin's
// adapter.start. A plugin that does not implement adapter.start is
// treated as ready — many transports have nothing to do here.
func (a *Adapter) Start(ctx context.Context) error {
	if err := a.client.Subscribe(ctx, sidecar.Filter(TopicEvent)); err != nil {
		return fmt.Errorf("bridge: subscribe to %s: %w", TopicEvent, err)
	}
	if err := a.client.Call(ctx, MethodStart, nil, nil); err != nil {
		if !isMethodUnsupported(err) {
			return fmt.Errorf("bridge: %s: %w", MethodStart, err)
		}
	}
	return nil
}

// Stop calls adapter.stop, ignoring "method not found" for the same reason
// as Start. The manager still owns the sidecar client and will close it.
func (a *Adapter) Stop(ctx context.Context) error {
	if err := a.client.Call(ctx, MethodStop, nil, nil); err != nil {
		if !isMethodUnsupported(err) {
			return fmt.Errorf("bridge: %s: %w", MethodStop, err)
		}
	}
	return nil
}

// Discover returns a channel over the snapshot the plugin reports. It is
// closed synchronously — Discover is a one-shot on this contract, not a
// long-running stream. Adapters that stream discoveries (BLE, mDNS)
// belong on a separate topic once that surface is agreed.
func (a *Adapter) Discover(ctx context.Context) (<-chan ports.DiscoveredDevice, error) {
	var res DiscoverResult
	if err := a.client.Call(ctx, MethodDiscover, nil, &res); err != nil {
		return nil, fmt.Errorf("bridge: %s: %w", MethodDiscover, err)
	}
	out := make(chan ports.DiscoveredDevice, len(res.Devices))
	for _, d := range res.Devices {
		out <- ports.DiscoveredDevice{
			TransportRef: d.TransportRef,
			Type:         d.Type,
			Name:         d.Name,
			Manufacturer: d.Manufacturer,
			Model:        d.Model,
			Features:     d.Features,
			Metadata:     d.Metadata,
		}
	}
	close(out)
	return out, nil
}

// Commission asks the plugin to add a device. Progress reporting is
// deferred — req.Progress is dropped until the topic contract lands.
func (a *Adapter) Commission(ctx context.Context, req ports.CommissionRequest) (domain.TransportRef, error) {
	params := CommissionParams{
		Payload:  req.Payload,
		WifiSSID: req.WifiSSID,
		WifiCred: req.WifiCred,
		Extra:    req.Extra,
	}
	var res CommissionResult
	if err := a.client.Call(ctx, MethodCommission, params, &res); err != nil {
		return "", fmt.Errorf("bridge: %s: %w", MethodCommission, err)
	}
	return res.Ref, nil
}

// ReadState fetches one attribute.
func (a *Adapter) ReadState(ctx context.Context, ref domain.TransportRef, feature domain.FeatureKey, key domain.StateKey) (any, error) {
	params := ReadStateParams{AttrRef: AttrRef{Ref: ref, Feature: feature, Key: string(key)}}
	var res ReadStateResult
	if err := a.client.Call(ctx, MethodReadState, params, &res); err != nil {
		return nil, fmt.Errorf("bridge: %s: %w", MethodReadState, err)
	}
	return res.Value, nil
}

// WriteState requests a change to a settable attribute.
func (a *Adapter) WriteState(ctx context.Context, ref domain.TransportRef, feature domain.FeatureKey, key domain.StateKey, value any) error {
	params := WriteStateParams{
		AttrRef: AttrRef{Ref: ref, Feature: feature, Key: string(key)},
		Value:   value,
	}
	if err := a.client.Call(ctx, MethodWriteState, params, nil); err != nil {
		return fmt.Errorf("bridge: %s: %w", MethodWriteState, err)
	}
	return nil
}

// InvokeAction runs a command against a feature.
func (a *Adapter) InvokeAction(ctx context.Context, ref domain.TransportRef, feature domain.FeatureKey, action domain.ActionKey, params map[string]any) error {
	if err := a.client.Call(ctx, MethodInvoke, InvokeParams{Ref: ref, Feature: feature, Action: action, Args: params}, nil); err != nil {
		return fmt.Errorf("bridge: %s: %w", MethodInvoke, err)
	}
	return nil
}

// Subscribe returns a fan-out of every push on the adapter.event topic,
// translated to ports.TransportEvent. Closes when ctx is done.
//
// The channel is buffered so a caller that briefly stops reading does not
// stall the sidecar's push pump. Longer stalls are the caller's problem.
func (a *Adapter) Subscribe(ctx context.Context) (<-chan ports.TransportEvent, error) {
	out := make(chan ports.TransportEvent, 64)
	pushes := a.client.Pushes()

	go func() {
		defer close(out)
		for {
			select {
			case <-ctx.Done():
				return
			case p, ok := <-pushes:
				if !ok {
					return
				}
				if string(p.Topic) != TopicEvent {
					continue
				}
				var ev EventPayload
				if err := json.Unmarshal(p.Payload, &ev); err != nil {
					a.log.Warn("bridge: dropping malformed event", "err", err)
					continue
				}
				select {
				case out <- ports.TransportEvent{
					Ref:     ev.Ref,
					Kind:    ev.Kind,
					Feature: ev.Feature,
					Key:     ev.Key,
					Value:   ev.Value,
				}:
				case <-ctx.Done():
					return
				}
			}
		}
	}()
	return out, nil
}

// Decommission removes a device from the plugin's fabric.
func (a *Adapter) Decommission(ctx context.Context, ref domain.TransportRef) error {
	if err := a.client.Call(ctx, MethodDecommission, DecommissionParams{Ref: ref}, nil); err != nil {
		return fmt.Errorf("bridge: %s: %w", MethodDecommission, err)
	}
	return nil
}

// isMethodUnsupported reports whether the sidecar answered "method not
// found". Start and Stop tolerate that; every other call surfaces it as
// an ordinary failure.
func isMethodUnsupported(err error) bool {
	var rpc *sidecar.Error
	if errors.As(err, &rpc) {
		return rpc.Code == sidecar.CodeProtocolUnsupported
	}
	return false
}
