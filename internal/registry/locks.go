// Package registry maintains the in-memory view of devices and provides
// per-device mutual exclusion for write operations.
package registry

import (
	"sync"

	"github.com/kliuchnikovv/keystone/internal/domain"
)

// KeyedLock is a mutex-per-key primitive. It gives the rules engine and the
// service layer a way to guarantee "only one write to a given device at a
// time" without holding a global lock.
//
// This is our answer to Home Assistant's mode: queued dance — deterministic
// serialization per device is baked in, not opt-in per rule.
type KeyedLock struct {
	mu    sync.Mutex
	locks map[domain.DeviceID]*deviceLock
}

type deviceLock struct {
	mu       sync.Mutex
	refCount int
}

// NewKeyedLock returns an empty lock table.
func NewKeyedLock() *KeyedLock {
	return &KeyedLock{
		locks: make(map[domain.DeviceID]*deviceLock),
	}
}

// Acquire locks the mutex for id and returns a release function. The
// release must be called exactly once.
//
// Idiomatic usage:
//
//	release := kl.Acquire(id)
//	defer release()
//	// safe to mutate device state here
func (kl *KeyedLock) Acquire(id domain.DeviceID) func() {
	kl.mu.Lock()
	entry, ok := kl.locks[id]
	if !ok {
		entry = &deviceLock{}
		kl.locks[id] = entry
	}
	entry.refCount++
	kl.mu.Unlock()

	entry.mu.Lock()

	return func() {
		entry.mu.Unlock()

		kl.mu.Lock()
		entry.refCount--
		if entry.refCount == 0 {
			delete(kl.locks, id)
		}
		kl.mu.Unlock()
	}
}
