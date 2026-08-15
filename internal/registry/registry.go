package registry

import (
	"errors"
	"sync"
	"time"

	"github.com/kliuchnikovv/keystone/internal/domain"
)

// ErrNotFound is returned when a device is not present in the registry.
var ErrNotFound = errors.New("device not found")

// ErrAlreadyExists is returned when trying to add a device with an ID that
// already exists.
var ErrAlreadyExists = errors.New("device already exists")

// Registry is the in-memory device catalogue. It is the authoritative source
// of truth for "what devices does this engine manage right now"; storage
// backs it up on disk.
type Registry struct {
	mu      sync.RWMutex
	devices map[domain.DeviceID]*domain.Device
	states  map[stateKey]domain.StateSnapshot
	locks   *KeyedLock
}

type stateKey struct {
	DeviceID domain.DeviceID
	Feature  domain.FeatureKey
	Key      domain.StateKey
}

// New returns an empty Registry.
func New() *Registry {
	return &Registry{
		devices: make(map[domain.DeviceID]*domain.Device),
		states:  make(map[stateKey]domain.StateSnapshot),
		locks:   NewKeyedLock(),
	}
}

// Add inserts a device. Returns ErrAlreadyExists if the ID is taken.
func (r *Registry) Add(d *domain.Device) error {
	r.mu.Lock()
	defer r.mu.Unlock()

	if _, exists := r.devices[d.ID]; exists {
		return ErrAlreadyExists
	}
	if d.CreatedAt.IsZero() {
		d.CreatedAt = time.Now().UTC()
	}
	d.UpdatedAt = time.Now().UTC()
	r.devices[d.ID] = d
	return nil
}

// Get returns a copy of the device (defensive) or ErrNotFound.
func (r *Registry) Get(id domain.DeviceID) (*domain.Device, error) {
	r.mu.RLock()
	defer r.mu.RUnlock()

	d, ok := r.devices[id]
	if !ok {
		return nil, ErrNotFound
	}
	cp := *d
	return &cp, nil
}

// List returns copies of all devices, ordered by CreatedAt.
func (r *Registry) List() []*domain.Device {
	r.mu.RLock()
	defer r.mu.RUnlock()

	out := make([]*domain.Device, 0, len(r.devices))
	for _, d := range r.devices {
		cp := *d
		out = append(out, &cp)
	}
	return out
}

// UpdateDiscoveryInfo refreshes the discovery-derived fields (Features, Type,
// Manufacturer, Model) on an already-known device. Name is preserved because
// the user may have renamed the device. Returns true if the device existed
// and something actually changed.
func (r *Registry) UpdateDiscoveryInfo(id domain.DeviceID, deviceType domain.DeviceType, manufacturer, model string, features []domain.Feature) bool {
	r.mu.Lock()
	defer r.mu.Unlock()

	d, ok := r.devices[id]
	if !ok {
		return false
	}
	changed := false
	if deviceType != "" && d.Type != deviceType {
		d.Type = deviceType
		changed = true
	}
	if manufacturer != "" && d.Manufacturer != manufacturer {
		d.Manufacturer = manufacturer
		changed = true
	}
	if model != "" && d.Model != model {
		d.Model = model
		changed = true
	}
	if len(features) > 0 && !featuresEqual(d.Features, features) {
		d.Features = features
		changed = true
	}
	if changed {
		d.UpdatedAt = time.Now().UTC()
	}
	return changed
}

func featuresEqual(a, b []domain.Feature) bool {
	if len(a) != len(b) {
		return false
	}
	for i := range a {
		if a[i].Key != b[i].Key {
			return false
		}
	}
	return true
}

// Remove deletes the device and any state associated with it.
func (r *Registry) Remove(id domain.DeviceID) error {
	r.mu.Lock()
	defer r.mu.Unlock()

	if _, ok := r.devices[id]; !ok {
		return ErrNotFound
	}
	delete(r.devices, id)
	for k := range r.states {
		if k.DeviceID == id {
			delete(r.states, k)
		}
	}
	return nil
}

// PutState updates the current-state cache for one attribute.
func (r *Registry) PutState(s domain.StateSnapshot) {
	r.mu.Lock()
	defer r.mu.Unlock()

	key := stateKey{s.DeviceID, s.Feature, s.Key}
	r.states[key] = s
}

// GetState returns the last-known snapshot or false if never observed.
func (r *Registry) GetState(id domain.DeviceID, feat domain.FeatureKey, key domain.StateKey) (domain.StateSnapshot, bool) {
	r.mu.RLock()
	defer r.mu.RUnlock()

	s, ok := r.states[stateKey{id, feat, key}]
	return s, ok
}

// Lock returns a per-device release function. Callers must hold this before
// mutating device state; see docs on KeyedLock.
func (r *Registry) Lock(id domain.DeviceID) func() {
	return r.locks.Acquire(id)
}
