// Package manager owns the runtime lifecycle of installed plugins.
//
// Manager glues registry (what is installed) to supervisor (what is
// running). It answers /plugins/* requests: list, enable, disable, health.
// Install and uninstall — file-level operations — go through a Store
// component that hands the manager freshly-written directories; that
// glue lives in a future package.
package manager

import (
	"context"
	"errors"
	"fmt"
	"log/slog"
	"os"
	"path/filepath"
	"sync"

	"github.com/kliuchnikovv/keystone-api/sidecar"
	"github.com/kliuchnikovv/keystone/internal/plugin"
	"github.com/kliuchnikovv/keystone/internal/plugin/registry"
	"github.com/kliuchnikovv/keystone/internal/plugin/store"
	"github.com/kliuchnikovv/keystone/internal/plugin/supervisor"
)

// State is a plugin's lifecycle position. Values line up with the
// plugin-store-architecture doc: Discovered before enable, Running while
// the entrypoint is up and connected, Stopped after a clean disable,
// Failed when the last enable attempt errored (manifest broken, spawn
// failed, handshake refused).
type State string

const (
	StateDiscovered State = "discovered"
	StateRunning    State = "running"
	StateStopped    State = "stopped"
	StateFailed     State = "failed"
)

// Options configures a Manager.
type Options struct {
	Registry *registry.Registry
	Logger   *slog.Logger

	// DataDir is the base under which each plugin gets its own persistent
	// data directory, exposed to the entrypoint and its sidecars as
	// $KEYSTONE_PLUGIN_DATA. Defaults to <registry-root>/../plugin-data.
	DataDir string

	// StateStore persists which plugins the operator has enabled so the
	// daemon can bring them back up on restart. Nil disables persistence
	// (useful for tests). Call RestoreEnabled after Discover to apply.
	StateStore StateStore

	// TrustedRegistries is an allowlist of registry base URLs. Empty
	// means "no allowlist" — install accepts any URL that passes the
	// store's own scheme+host validation, which is fine for
	// development but should be filled on any shared deployment to
	// prevent an operator (or an SSRF vector) from pointing install
	// at an arbitrary host.
	TrustedRegistries []string

	// PluginVerifier gates every installed package. Defaults to
	// store.SHA256Verifier — swap in store.Ed25519Verifier (or a
	// future Sigstore verifier) for real signing.
	PluginVerifier store.Verifier

	// ClientOptions is the sidecar.CoreOptions template applied when a
	// plugin is enabled. Version and Handler are overridden per-plugin;
	// the rest (heartbeat, backoff, logger) are copied.
	ClientOptions sidecar.CoreOptions

	// OnPluginLog receives one log line at a time from each running
	// plugin, tagged with the plugin name and stream. Nil discards.
	OnPluginLog func(pluginName, stream, line string)

	// OnEnable fires once a plugin's handshake completes. Returning a
	// non-nil error rolls the enable back — the supervisor is stopped
	// and the plugin lands in Failed. Used by the daemon to plug the
	// plugin's client into service.DeviceService as an adapter.
	OnEnable func(ctx context.Context, name string, manifest *plugin.Manifest, client *sidecar.Client) error

	// OnDisable fires when a plugin transitions out of Running (Disable,
	// Shutdown, or a Failed transition). Called with the manifest so the
	// daemon can look the plugin's kind up again without touching the
	// registry. Called after the supervisor is stopped.
	OnDisable func(name string, manifest *plugin.Manifest)
}

// PluginStatus is the public view exposed via /plugins.
type PluginStatus struct {
	Name      string           `json:"name"`
	Version   string           `json:"version"`
	State     State            `json:"state"`
	Connected bool             `json:"connected"`
	LastError string           `json:"last_error,omitempty"`
	Manifest  *plugin.Manifest `json:"manifest,omitempty"`
}

// ErrPluginNotFound is returned when a plugin name is not known.
var ErrPluginNotFound = errors.New("plugin not found")

// Manager is the top-level plugin lifecycle owner.
type Manager struct {
	opts Options
	log  *slog.Logger

	mu      sync.RWMutex
	entries map[string]*record

	// logs holds a bounded ring of recent lines per plugin, plus any
	// followers streaming live. Guarded by logsMu.
	logsMu sync.RWMutex
	logs   map[string]*logBuffer
}

type record struct {
	entry     registry.Entry
	state     State
	lastError error
	sv        *supervisor.Supervisor
	sidecars  []*supervisor.Process
	client    *sidecar.Client
}

// New builds a Manager. Discover must be called to populate it.
func New(opts Options) (*Manager, error) {
	if opts.Registry == nil {
		return nil, errors.New("manager: Options.Registry is required")
	}
	if opts.Logger == nil {
		opts.Logger = slog.Default()
	}
	return &Manager{
		opts:    opts,
		log:     opts.Logger,
		entries: make(map[string]*record),
		logs:    make(map[string]*logBuffer),
	}, nil
}

// Discover scans the registry and rebuilds the manager's view. Existing
// running records are preserved; new plugins land as Discovered; a
// plugin that disappeared from disk while running keeps its record so
// health surfaces the drift (manager will refuse a subsequent enable).
func (m *Manager) Discover() error {
	entries, err := m.opts.Registry.Discover()
	if err != nil {
		return err
	}
	m.mu.Lock()
	defer m.mu.Unlock()

	seen := make(map[string]bool, len(entries))
	for _, e := range entries {
		name := e.Name()
		seen[name] = true
		if r, ok := m.entries[name]; ok {
			// Refresh the manifest but keep runtime state as-is.
			r.entry = e
			if e.Err != nil && r.state != StateRunning {
				r.state = StateFailed
				r.lastError = e.Err
			}
			continue
		}
		state := StateDiscovered
		if e.Err != nil {
			state = StateFailed
		}
		m.entries[name] = &record{entry: e, state: state, lastError: e.Err}
	}
	// A plugin whose directory vanished while stopped can be dropped —
	// while running, we keep it so /plugins reports the drift honestly.
	for name, r := range m.entries {
		if seen[name] {
			continue
		}
		if r.state == StateRunning {
			continue
		}
		delete(m.entries, name)
	}
	return nil
}

// List returns the current view, in the order Discover produced.
func (m *Manager) List() []PluginStatus {
	m.mu.RLock()
	defer m.mu.RUnlock()
	out := make([]PluginStatus, 0, len(m.entries))
	for _, r := range m.entries {
		out = append(out, m.statusLocked(r))
	}
	return out
}

// Get reports one plugin. Returns false when the name is unknown.
func (m *Manager) Get(name string) (PluginStatus, bool) {
	m.mu.RLock()
	defer m.mu.RUnlock()
	r, ok := m.entries[name]
	if !ok {
		return PluginStatus{}, false
	}
	return m.statusLocked(r), true
}

// PluginDir returns the directory the plugin was installed into, or
// "" when the name is unknown. Used by the /plugins/{name}/ui/*
// static server to resolve asset paths.
func (m *Manager) PluginDir(name string) string {
	m.mu.RLock()
	defer m.mu.RUnlock()
	r, ok := m.entries[name]
	if !ok {
		return ""
	}
	return r.entry.Dir
}

// Client returns the live sidecar client for a running plugin, or nil.
// Used by ports.PluginAdapter to route Adapter calls.
func (m *Manager) Client(name string) *sidecar.Client {
	m.mu.RLock()
	defer m.mu.RUnlock()
	r, ok := m.entries[name]
	if !ok {
		return nil
	}
	return r.client
}

// Enable spawns the entrypoint of a discovered plugin and completes the
// sidecar handshake. Enabling a plugin twice is a no-op.
func (m *Manager) Enable(ctx context.Context, name string) error {
	m.mu.Lock()
	r, ok := m.entries[name]
	if !ok {
		m.mu.Unlock()
		return fmt.Errorf("manager: unknown plugin %q", name)
	}
	if r.state == StateRunning {
		m.mu.Unlock()
		return nil
	}
	if r.entry.Err != nil || r.entry.Manifest == nil {
		m.mu.Unlock()
		return fmt.Errorf("manager: cannot enable %q: %w", name, r.entry.Err)
	}
	ep := r.entry.Manifest.Spec.Entrypoint
	if ep == nil || ep.Exec == "" {
		m.mu.Unlock()
		return fmt.Errorf("manager: plugin %q has no entrypoint", name)
	}

	// Refuse to enable a plugin whose deps are not themselves running or
	// discovered — a Matter plugin cannot start without secretstore.
	for _, dep := range r.entry.Manifest.Spec.DependsOn {
		dr, ok := m.entries[dep]
		if !ok {
			m.mu.Unlock()
			return fmt.Errorf("manager: plugin %q depends on missing %q", name, dep)
		}
		if dr.state == StateFailed {
			m.mu.Unlock()
			return fmt.Errorf("manager: plugin %q depends on failed %q", name, dep)
		}
	}
	m.mu.Unlock()

	dataDir := m.dataDirFor(name)
	if err := os.MkdirAll(dataDir, 0o755); err != nil {
		m.markFailed(name, err)
		return fmt.Errorf("manager: creating data dir for %q: %w", name, err)
	}

	// Base env passed to entrypoint and to every declared sidecar so
	// they can find each other on the filesystem and the manifest's
	// $KEYSTONE_PLUGIN_* references expand consistently.
	baseEnv := map[string]string{
		"KEYSTONE_PLUGIN_NAME": name,
		"KEYSTONE_PLUGIN_DATA": dataDir,
	}

	// Bring up manifest-declared sidecars first — the entrypoint may
	// depend on them (Matter's matter-server is the canonical case).
	// Stop them in reverse if the entrypoint fails to come up.
	sidecars, err := m.startSidecars(name, r.entry.Dir, r.entry.Manifest.Spec.Sidecars, baseEnv)
	if err != nil {
		m.markFailed(name, err)
		return err
	}

	cfg := supervisor.Config{
		Name:    r.entry.Manifest.Metadata.Name,
		Version: r.entry.Manifest.Metadata.Version,
		Exec:    append([]string{resolveExec(r.entry.Dir, ep.Exec)}, ep.Args...),
		WorkDir: r.entry.Dir,
		Env:     entrypointEnv(ep.Env, baseEnv),
		Restart: mapRestart(r.entry.Manifest),
		Logger:  m.log,
		OnStdout: m.captureLine(name, "stdout"),
		OnStderr: m.captureLine(name, "stderr"),
		Client: m.opts.ClientOptions,
	}
	sv, err := supervisor.New(cfg)
	if err != nil {
		stopAll(sidecars)
		m.markFailed(name, err)
		return err
	}
	client, err := sv.Start(ctx)
	if err != nil {
		stopAll(sidecars)
		m.markFailed(name, err)
		return err
	}

	if m.opts.OnEnable != nil {
		if err := m.opts.OnEnable(ctx, name, r.entry.Manifest, client); err != nil {
			// Roll back: the plugin process is up but the daemon refused
			// to adopt it. Landing in Failed is the honest signal — the
			// operator can retry after fixing the mount error.
			_ = sv.Stop(context.Background())
			stopAll(sidecars)
			m.markFailed(name, err)
			return fmt.Errorf("manager: OnEnable %q: %w", name, err)
		}
	}

	m.mu.Lock()
	r = m.entries[name]
	r.sv = sv
	r.sidecars = sidecars
	r.client = client
	r.state = StateRunning
	r.lastError = nil
	m.mu.Unlock()
	m.persistState()
	return nil
}

// Disable stops a running plugin. Safe to call on any state.
func (m *Manager) Disable(ctx context.Context, name string) error {
	m.mu.Lock()
	r, ok := m.entries[name]
	if !ok {
		m.mu.Unlock()
		return fmt.Errorf("manager: unknown plugin %q", name)
	}
	sv := r.sv
	sidecars := r.sidecars
	wasRunning := r.state == StateRunning
	manifest := r.entry.Manifest
	r.sv = nil
	r.sidecars = nil
	r.client = nil
	if wasRunning {
		r.state = StateStopped
	}
	m.mu.Unlock()

	if sv == nil && len(sidecars) == 0 {
		return nil
	}
	var err error
	if sv != nil {
		err = sv.Stop(ctx)
	}
	stopAll(sidecars)
	if wasRunning && m.opts.OnDisable != nil {
		m.opts.OnDisable(name, manifest)
	}
	if wasRunning {
		m.persistState()
	}
	return err
}

// Shutdown disables every running plugin. Best-effort — errors are logged
// but a plugin that refuses to stop does not block the others.
func (m *Manager) Shutdown(ctx context.Context) {
	m.mu.RLock()
	names := make([]string, 0, len(m.entries))
	for name := range m.entries {
		names = append(names, name)
	}
	m.mu.RUnlock()
	for _, name := range names {
		if err := m.Disable(ctx, name); err != nil {
			m.log.Warn("plugin disable during shutdown", "plugin", name, "err", err)
		}
	}
}

func (m *Manager) markFailed(name string, err error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	if r, ok := m.entries[name]; ok {
		r.state = StateFailed
		r.lastError = err
	}
}

func (m *Manager) statusLocked(r *record) PluginStatus {
	st := PluginStatus{
		Name:     r.entry.Name(),
		State:    r.state,
		Manifest: r.entry.Manifest,
	}
	if r.entry.Manifest != nil {
		st.Version = r.entry.Manifest.Metadata.Version
	}
	if r.client != nil {
		st.Connected = r.client.Connected()
	}
	if r.lastError != nil {
		st.LastError = r.lastError.Error()
	}
	return st
}

// resolveExec makes a manifest's entrypoint.exec absolute against the
// plugin's install dir. The manifest schema requires exec to be a path
// relative to the plugin directory, so any non-absolute value gets joined
// unconditionally. This matches the promise made to plugin authors.
func resolveExec(dir, exec string) string {
	if filepath.IsAbs(exec) {
		return exec
	}
	return filepath.Join(dir, exec)
}

// mapRestart pulls the effective restart policy from the manifest. The
// manifest carries per-sidecar restart; the entrypoint itself has no
// explicit field, so we treat it as on-failure by default.
func mapRestart(_ *plugin.Manifest) supervisor.RestartPolicy {
	return supervisor.RestartOnFailure
}
