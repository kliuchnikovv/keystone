//go:build !grpc

package main

import (
	"context"
	"log/slog"

	"github.com/kliuchnikovv/keystone/internal/ports"
	"github.com/kliuchnikovv/keystone/internal/registry"
	"github.com/kliuchnikovv/keystone/internal/rules"
	"github.com/kliuchnikovv/keystone/internal/service"
)

// startGRPC is a no-op when the `grpc` build tag is not set.
// Rebuild with `make build-grpc` (or `go build -tags=grpc ./cmd/keystone`) to
// enable the gRPC server. See internal/api/grpcapi/server.go.
func startGRPC(ctx context.Context, log *slog.Logger, addr string,
	_ *service.DeviceService, _ *registry.Registry, _ ports.EventBus, _ *rules.Engine) {
	if addr != "" {
		log.Warn("grpc requested but binary built without -tags=grpc; ignoring", "addr", addr)
	}
}
