package ports

import (
	"context"

	"github.com/kliuchnikovv/keystone/internal/domain"
)

// DeviceRepository persists devices and their features. Implementations live
// under internal/storage/sqlite (and could later add postgres, in-memory, etc.).
type DeviceRepository interface {
	Save(ctx context.Context, d *domain.Device) error
	Get(ctx context.Context, id domain.DeviceID) (*domain.Device, error)
	List(ctx context.Context) ([]*domain.Device, error)
	Delete(ctx context.Context, id domain.DeviceID) error
}
