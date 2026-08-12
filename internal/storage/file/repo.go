// Package file provides a JSON-file-backed implementation of the storage
// repositories. Zero external dependencies; suitable for POC and demos.
// Production will swap this for SQLite behind the same interface.
package file

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"sync"

	"github.com/kliuchnikovv/keystone/internal/domain"
)

// ErrNotFound is returned when a lookup misses.
var ErrNotFound = errors.New("not found")

// Repo is a single JSON file per collection. Reads are cheap (mmap'd if we
// wanted), writes take a global mutex and rewrite atomically.
type Repo struct {
	dir string
	mu  sync.Mutex

	devices map[domain.DeviceID]*domain.Device
	rules   map[domain.RuleID]*domain.Rule
}

// Open loads state from dir (creates it if missing).
func Open(dir string) (*Repo, error) {
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return nil, fmt.Errorf("mkdir %s: %w", dir, err)
	}
	r := &Repo{
		dir:     dir,
		devices: make(map[domain.DeviceID]*domain.Device),
		rules:   make(map[domain.RuleID]*domain.Rule),
	}
	if err := r.loadDevices(); err != nil {
		return nil, err
	}
	if err := r.loadRules(); err != nil {
		return nil, err
	}
	return r, nil
}

// --- Devices ---

// SaveDevice persists a device.
func (r *Repo) SaveDevice(ctx context.Context, d *domain.Device) error {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.devices[d.ID] = d
	return r.writeDevices()
}

// GetDevice returns a device by id.
func (r *Repo) GetDevice(ctx context.Context, id domain.DeviceID) (*domain.Device, error) {
	r.mu.Lock()
	defer r.mu.Unlock()
	d, ok := r.devices[id]
	if !ok {
		return nil, ErrNotFound
	}
	cp := *d
	return &cp, nil
}

// ListDevices returns copies of all devices.
func (r *Repo) ListDevices(ctx context.Context) ([]*domain.Device, error) {
	r.mu.Lock()
	defer r.mu.Unlock()
	out := make([]*domain.Device, 0, len(r.devices))
	for _, d := range r.devices {
		cp := *d
		out = append(out, &cp)
	}
	return out, nil
}

// DeleteDevice removes a device.
func (r *Repo) DeleteDevice(ctx context.Context, id domain.DeviceID) error {
	r.mu.Lock()
	defer r.mu.Unlock()
	if _, ok := r.devices[id]; !ok {
		return ErrNotFound
	}
	delete(r.devices, id)
	return r.writeDevices()
}

// --- Rules ---

// Save persists a rule.
func (r *Repo) Save(ctx context.Context, rule *domain.Rule) error {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.rules[rule.ID] = rule
	return r.writeRules()
}

// List returns copies of all rules.
func (r *Repo) List(ctx context.Context) ([]*domain.Rule, error) {
	r.mu.Lock()
	defer r.mu.Unlock()
	out := make([]*domain.Rule, 0, len(r.rules))
	for _, rule := range r.rules {
		cp := *rule
		out = append(out, &cp)
	}
	return out, nil
}

// Get returns a rule by id.
func (r *Repo) Get(ctx context.Context, id domain.RuleID) (*domain.Rule, error) {
	r.mu.Lock()
	defer r.mu.Unlock()
	rule, ok := r.rules[id]
	if !ok {
		return nil, ErrNotFound
	}
	cp := *rule
	return &cp, nil
}

// Delete removes a rule.
func (r *Repo) Delete(ctx context.Context, id domain.RuleID) error {
	r.mu.Lock()
	defer r.mu.Unlock()
	if _, ok := r.rules[id]; !ok {
		return ErrNotFound
	}
	delete(r.rules, id)
	return r.writeRules()
}

// --- File I/O (write-through, atomic rename) ---

func (r *Repo) devicesPath() string { return filepath.Join(r.dir, "devices.json") }
func (r *Repo) rulesPath() string   { return filepath.Join(r.dir, "rules.json") }

func (r *Repo) loadDevices() error {
	data, err := os.ReadFile(r.devicesPath())
	if errors.Is(err, os.ErrNotExist) {
		return nil
	}
	if err != nil {
		return err
	}
	var arr []*domain.Device
	if err := json.Unmarshal(data, &arr); err != nil {
		return fmt.Errorf("decode devices.json: %w", err)
	}
	for _, d := range arr {
		r.devices[d.ID] = d
	}
	return nil
}

func (r *Repo) loadRules() error {
	data, err := os.ReadFile(r.rulesPath())
	if errors.Is(err, os.ErrNotExist) {
		return nil
	}
	if err != nil {
		return err
	}
	var arr []*domain.Rule
	if err := json.Unmarshal(data, &arr); err != nil {
		return fmt.Errorf("decode rules.json: %w", err)
	}
	for _, rule := range arr {
		r.rules[rule.ID] = rule
	}
	return nil
}

func (r *Repo) writeDevices() error {
	arr := make([]*domain.Device, 0, len(r.devices))
	for _, d := range r.devices {
		arr = append(arr, d)
	}
	return atomicWriteJSON(r.devicesPath(), arr)
}

func (r *Repo) writeRules() error {
	arr := make([]*domain.Rule, 0, len(r.rules))
	for _, rule := range r.rules {
		arr = append(arr, rule)
	}
	return atomicWriteJSON(r.rulesPath(), arr)
}

func atomicWriteJSON(path string, v any) error {
	data, err := json.MarshalIndent(v, "", "  ")
	if err != nil {
		return err
	}
	tmp := path + ".tmp"
	if err := os.WriteFile(tmp, data, 0o644); err != nil {
		return err
	}
	return os.Rename(tmp, path)
}
