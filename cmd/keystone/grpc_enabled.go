//go:build grpc

package main

import (
	"context"
	"log/slog"

	"github.com/kliuchnikovv/keystone/internal/api/grpcapi"
	"github.com/kliuchnikovv/keystone/internal/ports"
	"github.com/kliuchnikovv/keystone/internal/registry"
	"github.com/kliuchnikovv/keystone/internal/rules"
	"github.com/kliuchnikovv/keystone/internal/service"
)

// startGRPC starts the gRPC server in a goroutine. The server exits when ctx
// is cancelled. Compiled only with `-tags=grpc`.
func startGRPC(ctx context.Context, log *slog.Logger, addr string,
	svc *service.DeviceService, reg *registry.Registry, bus ports.EventBus, eng *rules.Engine) {
	if addr == "" {
		return
	}
	server := grpcapi.New(log.With("component", "grpc"), svc, reg, bus, eng)
	go func() {
		if err := grpcapi.Serve(ctx, log.With("component", "grpc"), addr, server); err != nil {
			log.Error("grpc serve", "err", err)
		}
	}()
}
