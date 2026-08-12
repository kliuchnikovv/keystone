package ports

import (
	"context"

	"github.com/kliuchnikovv/keystone/internal/domain"
)

// EventBus is the in-process pub/sub for state changes and events.
// The bus is intentionally simple; consumers of higher-level abstractions
// (rules engine, WS server) subscribe to typed channels.
type EventBus interface {
	// PublishState notifies subscribers of a state snapshot.
	PublishState(ctx context.Context, s domain.StateSnapshot) error

	// PublishEvent notifies subscribers of a discrete event.
	PublishEvent(ctx context.Context, e domain.Event) error

	// SubscribeStates returns a channel of StateSnapshot values. Closing the
	// context unsubscribes.
	SubscribeStates(ctx context.Context) <-chan domain.StateSnapshot

	// SubscribeEvents returns a channel of Event values. Closing the context
	// unsubscribes.
	SubscribeEvents(ctx context.Context) <-chan domain.Event
}
