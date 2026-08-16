// Package sdk is the plugin author's front door.
//
// A transport plugin implements ports.Adapter and calls Run — the SDK
// registers every adapter.* handler, starts the adapter, pumps its
// events onto adapter.event, and hands progress callbacks to Commission
// callers. That is roughly 150 lines of boilerplate the plugin no longer
// has to carry.
//
// The SDK deliberately lives in this repo alongside plugins/matter.
// When keystone-api grows a stable public sdk package, this one will
// move there and shrink to a shim; for now every in-tree plugin
// imports it directly.
package sdk

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"log/slog"
	"os"
	"path/filepath"

	"github.com/kliuchnikovv/keystone-api/sidecar"

	"github.com/kliuchnikovv/keystone/internal/domain"
	"github.com/kliuchnikovv/keystone/internal/plugin/bridge"
	"github.com/kliuchnikovv/keystone/internal/ports"
)

// Adapter is the surface a transport plugin implements. Semantically
// identical to ports.Adapter — aliased so plugin code has one import to
// pin rather than reaching into keystone's internal tree.
type Adapter = ports.Adapter

// ErrorMapper translates a plugin-specific error into a sidecar.Error
// with the appropriate code. It is called for every non-nil error
// returned from the adapter — return nil to leave the error unchanged.
type ErrorMapper func(error) error

// Options configures Run.
type Options struct {
	// Name and Version identify the plugin in the handshake. Name must
	// match metadata.name in plugin.yaml — the manager cross-checks.
	Name    string
	Version string

	// Capabilities announces what the plugin can do. Must be a subset
	// of manifest.spec.capabilities; the manager rejects a superset.
	Capabilities []string

	// Adapter is the transport implementation. Its Kind() is not used
	// on the wire — the core reads the kind from the manifest — but
	// tests and internal logging use it.
	Adapter Adapter

	// ErrorMapper is applied to every error the adapter returns. Nil
	// means "pass through", which lets the bridge see the original
	// message but no sidecar code — a plugin whose errors are already
	// sidecar.Error can omit this.
	ErrorMapper ErrorMapper

	// Logger defaults to a stderr JSON handler at info.
	Logger *slog.Logger
}

// Run blocks for the life of the plugin. It reads the socket path from
// KEYSTONE_PLUGIN_SOCKET, spawns handlers for every adapter.* method,
// starts the adapter, pumps its Subscribe channel onto adapter.event,
// and returns when the core closes the connection.
//
// A plugin's main() typically ends with `log.Fatal(sdk.Run(ctx, opts))`.
func Run(ctx context.Context, opts Options) error {
	if opts.Name == "" {
		return errors.New("sdk: Options.Name is required")
	}
	if opts.Adapter == nil {
		return errors.New("sdk: Options.Adapter is required")
	}
	if opts.Logger == nil {
		opts.Logger = slog.New(slog.NewJSONHandler(os.Stderr, &slog.HandlerOptions{Level: slog.LevelInfo}))
	}
	log := opts.Logger.With("plugin", opts.Name)

	mux := sidecar.NewMux()
	registerHandlers(mux, opts.Adapter, opts.ErrorMapper, log)

	po := sidecar.PluginOptions{
		Name:         opts.Name,
		Version:      opts.Version,
		Capabilities: opts.Capabilities,
		Handler:      mux,
		Logger:       log,
		OnConnect: func(ctx context.Context, peer *sidecar.Peer) {
			if err := opts.Adapter.Start(ctx); err != nil {
				// A backend that is not yet reachable is not fatal —
				// the adapter is expected to keep retrying. We log
				// and keep the peer alive so the core sees typed
				// errors from later calls, not a lost plugin.
				log.Error("adapter start", "err", err)
			}
			pumpEvents(ctx, opts.Adapter, peer, log)
		},
		OnDisconnect: func(err error) {
			log.Info("core disconnected", "err", err)
		},
	}

	return sidecar.RunPlugin(ctx, po)
}

// pumpEvents forwards adapter events onto the bridge event topic. Runs
// for the life of the connection; a full outbound ring drops the event
// (logged) rather than blocking the pump.
func pumpEvents(ctx context.Context, adapter Adapter, peer *sidecar.Peer, log *slog.Logger) {
	events, err := adapter.Subscribe(ctx)
	if err != nil {
		log.Error("adapter subscribe", "err", err)
		return
	}
	for ev := range events {
		if err := peer.Publish(sidecar.Topic(bridge.TopicEvent), bridge.EventPayload{
			Ref:     ev.Ref,
			Kind:    ev.Kind,
			Feature: ev.Feature,
			Key:     ev.Key,
			Value:   ev.Value,
		}); err != nil {
			log.Warn("publish event dropped", "err", err, "kind", ev.Kind)
		}
	}
}

func mapErr(m ErrorMapper, err error) error {
	if err == nil || m == nil {
		return err
	}
	if mapped := m(err); mapped != nil {
		return mapped
	}
	return err
}

func registerHandlers(mux *sidecar.Mux, adapter Adapter, mapper ErrorMapper, log *slog.Logger) {
	mux.Handle(bridge.MethodStart, sidecar.HandlerFunc(func(_ context.Context, _ *sidecar.Request) (any, error) {
		// The adapter's Start was already called from OnConnect. Answering
		// ok here is honest and lets a caller Restart the adapter over
		// the wire without a reconnect.
		return map[string]any{}, nil
	}))

	mux.Handle(bridge.MethodStop, sidecar.HandlerFunc(func(ctx context.Context, _ *sidecar.Request) (any, error) {
		if err := adapter.Stop(ctx); err != nil {
			return nil, mapErr(mapper, err)
		}
		return map[string]any{}, nil
	}))

	mux.Handle(bridge.MethodDiscover, sidecar.HandlerFunc(func(ctx context.Context, _ *sidecar.Request) (any, error) {
		ch, err := adapter.Discover(ctx)
		if err != nil {
			return nil, mapErr(mapper, err)
		}
		var out bridge.DiscoverResult
		for d := range ch {
			out.Devices = append(out.Devices, bridge.DiscoveredDevice{
				TransportRef: d.TransportRef,
				Type:         d.Type,
				Name:         d.Name,
				Manufacturer: d.Manufacturer,
				Model:        d.Model,
				Features:     d.Features,
				Metadata:     d.Metadata,
			})
		}
		return out, nil
	}))

	mux.Handle(bridge.MethodCommission, sidecar.HandlerFunc(func(ctx context.Context, r *sidecar.Request) (any, error) {
		var p bridge.CommissionParams
		if err := r.Bind(&p); err != nil {
			return nil, err
		}
		var progress func(stage, message string)
		if p.ProgressID != "" {
			peer := r.Peer()
			progress = func(stage, message string) {
				log.Info("commission progress", "stage", stage, "message", message)
				if peer == nil {
					return
				}
				_ = peer.Publish(sidecar.Topic(bridge.TopicEvent), bridge.EventPayload{
					Kind:         bridge.KindCommissionProgress,
					CommissionID: p.ProgressID,
					Stage:        stage,
					Message:      message,
				})
			}
		}
		ref, err := adapter.Commission(ctx, ports.CommissionRequest{
			Payload:  p.Payload,
			WifiSSID: p.WifiSSID,
			WifiCred: p.WifiCred,
			Extra:    p.Extra,
			Progress: progress,
		})
		if err != nil {
			return nil, mapErr(mapper, err)
		}
		return bridge.CommissionResult{Ref: ref}, nil
	}))

	mux.Handle(bridge.MethodReadState, sidecar.HandlerFunc(func(ctx context.Context, r *sidecar.Request) (any, error) {
		var p bridge.ReadStateParams
		if err := r.Bind(&p); err != nil {
			return nil, err
		}
		v, err := adapter.ReadState(ctx, p.Ref, p.Feature, domain.StateKey(p.Key))
		if err != nil {
			return nil, mapErr(mapper, err)
		}
		return bridge.ReadStateResult{Value: v}, nil
	}))

	mux.Handle(bridge.MethodWriteState, sidecar.HandlerFunc(func(ctx context.Context, r *sidecar.Request) (any, error) {
		var p bridge.WriteStateParams
		if err := r.Bind(&p); err != nil {
			return nil, err
		}
		if err := adapter.WriteState(ctx, p.Ref, p.Feature, domain.StateKey(p.Key), p.Value); err != nil {
			return nil, mapErr(mapper, err)
		}
		return map[string]any{}, nil
	}))

	mux.Handle(bridge.MethodInvoke, sidecar.HandlerFunc(func(ctx context.Context, r *sidecar.Request) (any, error) {
		var p bridge.InvokeParams
		if err := r.Bind(&p); err != nil {
			return nil, err
		}
		if err := adapter.InvokeAction(ctx, p.Ref, p.Feature, p.Action, p.Args); err != nil {
			return nil, mapErr(mapper, err)
		}
		return map[string]any{}, nil
	}))

	mux.Handle(bridge.MethodDecommission, sidecar.HandlerFunc(func(ctx context.Context, r *sidecar.Request) (any, error) {
		var p bridge.DecommissionParams
		if err := r.Bind(&p); err != nil {
			return nil, err
		}
		if err := adapter.Decommission(ctx, p.Ref); err != nil {
			return nil, mapErr(mapper, err)
		}
		return map[string]any{}, nil
	}))
}

// LoadConfig reads the plugin's persisted config (written by the core
// via PUT /plugins/{name}/config) into v. Missing file returns nil so
// a first-run plugin can carry on with its defaults. The file lives at
// $KEYSTONE_PLUGIN_DATA/config.json — an absent env var is a
// configuration mistake in the manifest, and reported as such.
func LoadConfig(v any) error {
	dir := os.Getenv("KEYSTONE_PLUGIN_DATA")
	if dir == "" {
		return errors.New("sdk: KEYSTONE_PLUGIN_DATA is not set — is this running under the manager?")
	}
	raw, err := os.ReadFile(filepath.Join(dir, "config.json"))
	if err != nil {
		if os.IsNotExist(err) {
			return nil
		}
		return fmt.Errorf("sdk: read config: %w", err)
	}
	return json.Unmarshal(raw, v)
}
