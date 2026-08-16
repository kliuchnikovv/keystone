// keystone-plugin-matter is the Matter transport running out-of-process.
//
// Nothing here talks to the wire directly — sdk.Run does that. This
// binary just constructs a matter.Adapter and hands it over.
package main

import (
	"context"
	"errors"
	"log/slog"
	"os"

	"github.com/kliuchnikovv/keystone-api/sidecar"

	"github.com/kliuchnikovv/keystone/internal/adapters/matter"
	"github.com/kliuchnikovv/keystone/plugins/sdk"
)

// Version is stamped into hello so the manager's status output is
// meaningful. Bumped on protocol / config surface changes.
const Version = "0.1.0"

// envSidecarURL is set by the manifest so the operator does not repeat
// the URL on the CLI.
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

	wsClient := matter.NewWSClient(cfg.URL(), log)
	adapter := matter.New(log, cfg, wsClient)

	err = sdk.Run(context.Background(), sdk.Options{
		Name:         "matter",
		Version:      Version,
		Capabilities: []string{"transport.matter", "discover.mdns", "realtime.state-stream"},
		Adapter:      adapter,
		ErrorMapper:  mapMatterError,
		Logger:       log,
	})
	if err != nil {
		log.Error("plugin exited", "err", err)
		os.Exit(1)
	}
}

// mapMatterError translates the matter adapter's typed errors into the
// sidecar vocabulary the bridge understands. Kept plugin-local because
// only matter speaks these categories; other plugins pass their own
// mapper.
func mapMatterError(err error) error {
	if err == nil {
		return nil
	}
	switch matter.KindOf(err) {
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
	if errors.Is(err, matter.ErrSidecarUnavailable) || errors.Is(err, matter.ErrNotConnected) {
		return sidecar.Errorf(sidecar.CodePluginNotReady, "%s", err.Error())
	}
	return err
}
