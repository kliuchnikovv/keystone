// Package ws provides a tiny WebSocket-like server-sent-events endpoint for
// streaming state changes to UI clients. Uses HTTP long-lived response with
// chunked transfer + JSON lines (SSE-lite). Deliberately avoids external
// deps for POC; a real WebSocket via gorilla can drop in later.
package ws

import (
	"context"
	"encoding/json"
	"fmt"
	"log/slog"
	"net/http"

	"github.com/kliuchnikovv/keystone/internal/domain"
	"github.com/kliuchnikovv/keystone/internal/ports"
)

// Handler returns an http.HandlerFunc that streams state snapshots as
// newline-delimited JSON. Compatible with `curl -N`.
func Handler(bus ports.EventBus, log *slog.Logger) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		flusher, ok := w.(http.Flusher)
		if !ok {
			http.Error(w, "streaming unsupported", http.StatusInternalServerError)
			return
		}
		w.Header().Set("content-type", "application/x-ndjson")
		w.Header().Set("cache-control", "no-cache")
		w.Header().Set("connection", "keep-alive")
		w.WriteHeader(http.StatusOK)

		ctx, cancel := context.WithCancel(r.Context())
		defer cancel()

		stateCh := bus.SubscribeStates(ctx)
		eventCh := bus.SubscribeEvents(ctx)

		enc := json.NewEncoder(w)

		for {
			select {
			case <-ctx.Done():
				return
			case s, ok := <-stateCh:
				if !ok {
					return
				}
				if err := enc.Encode(envelope{Type: "state", Payload: s}); err != nil {
					log.Debug("state stream write", "err", err)
					return
				}
				flusher.Flush()
			case ev, ok := <-eventCh:
				if !ok {
					return
				}
				if err := enc.Encode(envelope{Type: "event", Payload: ev}); err != nil {
					log.Debug("event stream write", "err", err)
					return
				}
				flusher.Flush()
			}
		}
	}
}

type envelope struct {
	Type    string `json:"type"`
	Payload any    `json:"payload"`
}

// Unused import guard for domain — kept so future StateSnapshot references
// don't need re-import.
var _ = domain.StateSnapshot{}
var _ = fmt.Sprintf
