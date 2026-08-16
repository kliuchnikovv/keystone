// keystone-plugin-matter is the Matter transport running out-of-process.
//
// The core enables this plugin, the manager forks the binary and hands it
// its socket, and every adapter.* method the bridge sends here lands on the
// same internal/adapters/matter code that used to run in-tree. Nothing
// about the wire changes; only the topology does.
package main

import (
	"context"
	"errors"
	"fmt"
	"log/slog"
	"os"

	"github.com/kliuchnikovv/keystone-api/sidecar"

	"github.com/kliuchnikovv/keystone/internal/adapters/matter"
	"github.com/kliuchnikovv/keystone/internal/domain"
	"github.com/kliuchnikovv/keystone/internal/plugin/bridge"
	"github.com/kliuchnikovv/keystone/internal/ports"
)

// Version is stamped into hello so the manager's status output is
// meaningful. Bumped on protocol / config surface changes.
const Version = "0.1.0"

// envSidecarURL is set by the manifest so the operator does not repeat
// the URL on the CLI. Same convention as the in-tree -matter-sidecar
// flag on the core.
const envSidecarURL = "KEYSTONE_MATTER_SIDECAR_URL"

func main() {
	log := slog.New(slog.NewJSONHandler(os.Stderr, &slog.HandlerOptions{Level: slog.LevelInfo}))

	sidecarURL := os.Getenv(envSidecarURL)
	if sidecarURL == "" {
		log.Error("plugin misconfigured: " + envSidecarURL + " is required")
		os.Exit(2)
	}
	cfg, err := matter.ParseSidecarURL(sidecarURL)
	if err != nil {
		log.Error("plugin misconfigured", "err", err)
		os.Exit(2)
	}

	wsClient := matter.NewWSClient(fmt.Sprintf("ws://%s:%d", cfg.Host, cfg.Port), log)
	adapter := matter.New(log, cfg, wsClient)

	mux := sidecar.NewMux()
	registerHandlers(mux, adapter, log)

	opts := sidecar.PluginOptions{
		Name:         "matter",
		Version:      Version,
		Capabilities: []string{"transport.matter", "discover.mdns", "realtime.state-stream"},
		Handler:      mux,
		Logger:       log,
		OnConnect: func(ctx context.Context, peer *sidecar.Peer) {
			// Start pushes any adapter-status events; the pump below
			// forwards them onto the wire. Errors here mean the sidecar
			// is unreachable — Start reports them, we log and keep the
			// connection alive so the core can see plugin.not_ready
			// answers rather than losing the plugin entirely.
			if err := adapter.Start(ctx); err != nil {
				log.Error("matter adapter start", "err", err)
			}
			pumpEvents(ctx, adapter, peer, log)
		},
		OnDisconnect: func(err error) {
			log.Info("core disconnected", "err", err)
		},
	}

	if err := sidecar.RunPlugin(context.Background(), opts); err != nil {
		log.Error("plugin exited", "err", err)
		os.Exit(1)
	}
}

// pumpEvents forwards adapter events onto the bridge's adapter.event topic.
// Runs for the life of the connection.
func pumpEvents(ctx context.Context, adapter *matter.Adapter, peer *sidecar.Peer, log *slog.Logger) {
	events, err := adapter.Subscribe(ctx)
	if err != nil {
		log.Error("adapter subscribe", "err", err)
		return
	}
	for ev := range events {
		payload := bridge.EventPayload{
			Ref:     ev.Ref,
			Kind:    ev.Kind,
			Feature: ev.Feature,
			Key:     ev.Key,
			Value:   ev.Value,
		}
		if err := peer.Publish(sidecar.Topic(bridge.TopicEvent), payload); err != nil {
			// Publish errors on a live connection mean the ring is full
			// (a stuck subscriber) or the codec rejected the payload. We
			// log and drop; retrying inline would let one bad subscriber
			// stall every other one.
			log.Warn("publish event dropped", "err", err, "kind", ev.Kind)
		}
	}
}

// registerHandlers wires every adapter.* method to the matter adapter.
// The plugin does the same JSON-in / JSON-out marshalling the bridge does
// on the core side, so a change on either side surfaces here at compile
// time — the wire types live in internal/plugin/bridge.
func registerHandlers(mux *sidecar.Mux, adapter *matter.Adapter, log *slog.Logger) {
	mux.Handle(bridge.MethodStart, sidecar.HandlerFunc(func(ctx context.Context, _ *sidecar.Request) (any, error) {
		// Start is idempotent on the adapter — OnConnect already called
		// it — so answering ok here is honest, not a lie.
		return map[string]any{}, nil
	}))

	mux.Handle(bridge.MethodStop, sidecar.HandlerFunc(func(ctx context.Context, _ *sidecar.Request) (any, error) {
		if err := adapter.Stop(ctx); err != nil {
			return nil, err
		}
		return map[string]any{}, nil
	}))

	mux.Handle(bridge.MethodDiscover, sidecar.HandlerFunc(func(ctx context.Context, _ *sidecar.Request) (any, error) {
		ch, err := adapter.Discover(ctx)
		if err != nil {
			return nil, err
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
		ref, err := adapter.Commission(ctx, ports.CommissionRequest{
			Payload:  p.Payload,
			WifiSSID: p.WifiSSID,
			WifiCred: p.WifiCred,
			Extra:    p.Extra,
			Progress: func(stage, message string) {
				// Progress streaming across the bridge is not yet
				// modelled. Log locally so the operator can still see
				// pairing progress in plugin logs.
				log.Info("commission progress", "stage", stage, "message", message)
			},
		})
		if err != nil {
			return nil, mapError(err)
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
			return nil, mapError(err)
		}
		return bridge.ReadStateResult{Value: v}, nil
	}))

	mux.Handle(bridge.MethodWriteState, sidecar.HandlerFunc(func(ctx context.Context, r *sidecar.Request) (any, error) {
		var p bridge.WriteStateParams
		if err := r.Bind(&p); err != nil {
			return nil, err
		}
		if err := adapter.WriteState(ctx, p.Ref, p.Feature, domain.StateKey(p.Key), p.Value); err != nil {
			return nil, mapError(err)
		}
		return map[string]any{}, nil
	}))

	mux.Handle(bridge.MethodInvoke, sidecar.HandlerFunc(func(ctx context.Context, r *sidecar.Request) (any, error) {
		var p bridge.InvokeParams
		if err := r.Bind(&p); err != nil {
			return nil, err
		}
		if err := adapter.InvokeAction(ctx, p.Ref, p.Feature, p.Action, p.Args); err != nil {
			return nil, mapError(err)
		}
		return map[string]any{}, nil
	}))

	mux.Handle(bridge.MethodDecommission, sidecar.HandlerFunc(func(ctx context.Context, r *sidecar.Request) (any, error) {
		var p bridge.DecommissionParams
		if err := r.Bind(&p); err != nil {
			return nil, err
		}
		if err := adapter.Decommission(ctx, p.Ref); err != nil {
			return nil, mapError(err)
		}
		return map[string]any{}, nil
	}))
}

// mapError translates the matter adapter's typed errors into the sidecar
// vocabulary the bridge understands. Doing this at the boundary keeps
// error taxonomies isolated: the core does not need to import matter,
// and the plugin does not need to know sidecar codes anywhere but here.
func mapError(err error) error {
	if err == nil {
		return nil
	}
	kind := matter.KindOf(err)
	switch kind {
	case matter.ErrKindBadRequest:
		return sidecar.Errorf(sidecar.CodeValidationBadParams, "%s", err.Error())
	case matter.ErrKindNotFound:
		return sidecar.Errorf(sidecar.CodeDeviceNotFound, "%s", err.Error())
	case matter.ErrKindNotReady:
		return sidecar.Errorf(sidecar.CodePluginNotReady, "%s", err.Error())
	case matter.ErrKindUnreachable:
		return sidecar.Errorf(sidecar.CodeDeviceOffline, "%s", err.Error())
	case matter.ErrKindTimeout:
		return sidecar.Errorf(sidecar.CodeNetworkTimeout, "%s", err.Error())
	case matter.ErrKindUnsupported:
		return sidecar.Errorf(sidecar.CodeFeatureUnsupported, "%s", err.Error())
	case matter.ErrKindInternal:
		return sidecar.Errorf(sidecar.CodePluginInternal, "%s", err.Error())
	}
	// Unclassified transport failures — e.g. the WS to matter-server dropped
	// — surface as plugin.not_ready so IsRetryable on the core side keeps
	// the request in the retry envelope.
	if errors.Is(err, matter.ErrSidecarUnavailable) || errors.Is(err, matter.ErrNotConnected) {
		return sidecar.Errorf(sidecar.CodePluginNotReady, "%s", err.Error())
	}
	return err
}

