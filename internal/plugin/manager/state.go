package manager

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io/fs"
	"os"
	"path/filepath"
	"sync"
)

// StateStore persists which plugins the operator has enabled, so the
// daemon can bring them back up on restart without a POST /enable per
// plugin every boot.
//
// A minimal file-based default lives in FileStateStore; injecting a
// custom store lets a deployment fold this into an existing state
// backend (etcd, sqlite) if it grows one.
type StateStore interface {
	Load() (map[string]bool, error)
	Save(map[string]bool) error
}

// FileStateStore stores {name: enabled} as JSON at Path. Concurrent
// callers are serialised; the file is written atomically via
// write-then-rename so a torn write cannot leave the manager without a
// readable state at next boot.
type FileStateStore struct {
	Path string

	mu sync.Mutex
}

// Load reads the state file. A missing file is not an error — a fresh
// install has nothing enabled yet.
func (s *FileStateStore) Load() (map[string]bool, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	raw, err := os.ReadFile(s.Path)
	if err != nil {
		if errors.Is(err, fs.ErrNotExist) {
			return map[string]bool{}, nil
		}
		return nil, fmt.Errorf("plugin state: read %s: %w", s.Path, err)
	}
	var out map[string]bool
	if err := json.Unmarshal(raw, &out); err != nil {
		return nil, fmt.Errorf("plugin state: parse %s: %w", s.Path, err)
	}
	if out == nil {
		out = map[string]bool{}
	}
	return out, nil
}

// Save writes the state atomically.
func (s *FileStateStore) Save(state map[string]bool) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	if err := os.MkdirAll(filepath.Dir(s.Path), 0o755); err != nil {
		return err
	}
	raw, err := json.MarshalIndent(state, "", "  ")
	if err != nil {
		return err
	}
	tmp := s.Path + ".tmp"
	if err := os.WriteFile(tmp, raw, 0o644); err != nil {
		return err
	}
	return os.Rename(tmp, s.Path)
}

// persistState reads the current view and asks the store to record it.
// Called after every Enable / Disable; nil store is a no-op so tests
// and short-lived processes can skip persistence.
func (m *Manager) persistState() {
	if m.opts.StateStore == nil {
		return
	}
	m.mu.RLock()
	state := make(map[string]bool, len(m.entries))
	for name, r := range m.entries {
		state[name] = r.state == StateRunning
	}
	m.mu.RUnlock()
	if err := m.opts.StateStore.Save(state); err != nil {
		m.log.Warn("plugin state: save failed", "err", err)
	}
}

// RestoreEnabled enables every plugin the state store marks as enabled.
// Call this once at daemon start, after Discover, so operators do not
// have to POST /enable on every boot. Missing state, unknown plugins,
// and per-plugin enable failures are logged but do not stop the rest —
// one broken plugin should not keep the others down.
func (m *Manager) RestoreEnabled(ctx context.Context) {
	if m.opts.StateStore == nil {
		return
	}
	state, err := m.opts.StateStore.Load()
	if err != nil {
		m.log.Warn("plugin state: load failed", "err", err)
		return
	}
	enabled := make([]string, 0, len(state))
	for name, on := range state {
		if on {
			enabled = append(enabled, name)
		}
	}
	m.log.Info("plugin state: restoring", "count", len(enabled))
	for _, name := range enabled {
		if _, ok := m.Get(name); !ok {
			m.log.Warn("plugin state: enabled name not present on disk", "plugin", name)
			continue
		}
		if err := m.Enable(ctx, name); err != nil {
			m.log.Warn("plugin state: restore failed", "plugin", name, "err", err)
			continue
		}
		m.log.Info("plugin state: restored", "plugin", name)
	}
}
