// keystone-plugin-dirigera is the reference second plugin — proof that
// the SDK is not Matter-shaped. Talks HTTP+bearer to an IKEA DIRIGERA
// hub, no cloud round-trip.
//
// Config lives in $KEYSTONE_PLUGIN_DATA/config.json (managed via
// PUT /plugins/dirigera/config) and carries the hub URL, the app
// bearer token, and either a CA PEM or the hub's leaf certificate
// SHA-256 fingerprint. TLS verification is not skipped.
package main

import (
	"context"
	"errors"
	"log/slog"
	"os"

	"github.com/kliuchnikovv/keystone-api/sidecar"

	"github.com/kliuchnikovv/keystone/plugins/dirigera/internal/dirigera"
	"github.com/kliuchnikovv/keystone/plugins/sdk"
)

// Version is stamped into hello for the manager's status output.
const Version = "0.1.0"

// pluginConfig is the shape PUT /plugins/dirigera/config accepts.
// spec.config.schema in plugin.yaml validates the same fields.
type pluginConfig struct {
	HubURL       string `json:"hubUrl"`
	Token        string `json:"token"`
	CAPEM        string `json:"caPem,omitempty"`
	PinnedSHA256 string `json:"pinnedSha256,omitempty"`
}

func main() {
	log := slog.New(slog.NewJSONHandler(os.Stderr, &slog.HandlerOptions{Level: slog.LevelInfo}))

	var cfg pluginConfig
	if err := sdk.LoadConfig(&cfg); err != nil {
		log.Error("load config", "err", err)
		os.Exit(2)
	}
	if cfg.HubURL == "" || cfg.Token == "" {
		log.Error("plugin misconfigured: hubUrl and token are required — set them via PUT /plugins/dirigera/config")
		os.Exit(2)
	}
	if cfg.CAPEM == "" && cfg.PinnedSHA256 == "" {
		log.Error("plugin misconfigured: one of caPem or pinnedSha256 is required — the hub ships a self-signed cert and TLS must be verified")
		os.Exit(2)
	}

	client, err := dirigera.NewClient(dirigera.Config{
		BaseURL:      cfg.HubURL,
		Token:        cfg.Token,
		CAPEM:        []byte(cfg.CAPEM),
		PinnedSHA256: cfg.PinnedSHA256,
	})
	if err != nil {
		log.Error("dirigera client", "err", err)
		os.Exit(2)
	}
	adapter := dirigera.New(client, log)

	err = sdk.Run(context.Background(), sdk.Options{
		Name:         "dirigera",
		Version:      Version,
		Capabilities: []string{"transport.dirigera", "discover.mdns", "realtime.state-stream"},
		Adapter:      adapter,
		ErrorMapper:  mapDirigeraError,
		Logger:       log,
	})
	if err != nil {
		log.Error("plugin exited", "err", err)
		os.Exit(1)
	}
}

// mapDirigeraError translates our HTTP-shaped errors onto the sidecar
// vocabulary so the core sees typed categories and can decide whether
// to retry, warn the user, or surface for pairing.
func mapDirigeraError(err error) error {
	if err == nil {
		return nil
	}
	switch {
	case errors.Is(err, dirigera.ErrUnauthorized):
		return sidecar.Errorf(sidecar.CodeAuthInvalidCredentials, "%s", err.Error())
	case errors.Is(err, dirigera.ErrNotFound):
		return sidecar.Errorf(sidecar.CodeDeviceNotFound, "%s", err.Error())
	}
	// A transport-level failure (hub off the network, TLS handshake
	// refusal) lands as network.unavailable so IsRetryable keeps the
	// request in the retry envelope.
	return sidecar.Errorf(sidecar.CodeNetworkUnavailable, "%s", err.Error())
}
