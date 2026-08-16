package manager

import (
	"context"
	"errors"
	"fmt"
	"strings"

	"github.com/kliuchnikovv/keystone/internal/plugin/pack"
	"github.com/kliuchnikovv/keystone/internal/plugin/store"
)

// InstallRequest is what the /plugins/install HTTP handler passes in.
// Name is required; Version defaults to "latest"; Registry is the base
// URL of a store to fetch from. Force overwrites an existing installed
// dir.
type InstallRequest struct {
	Name     string
	Version  string
	Registry string
	Force    bool
}

// Install fetches, verifies and unpacks a plugin from a registry into
// the plugins root the manager was configured with, then rediscovers
// so the new plugin lands in the manager's view without a daemon
// restart.
//
// The plugin is left in Discovered state; call Enable to actually run
// it. That split lets an operator inspect the manifest, wire up
// config, and only then bring it up.
func (m *Manager) Install(ctx context.Context, req InstallRequest) (string, string, error) {
	if req.Name == "" {
		return "", "", errors.New("manager: Install: Name is required")
	}
	if req.Registry == "" {
		return "", "", errors.New("manager: Install: Registry is required")
	}
	if err := m.checkRegistryAllowlist(req.Registry); err != nil {
		return "", "", err
	}
	reg, err := store.New(req.Registry)
	if err != nil {
		return "", "", err
	}
	if m.opts.PluginVerifier != nil {
		reg = reg.WithVerifier(m.opts.PluginVerifier)
	}
	root := m.opts.Registry.Root()
	name, version, err := reg.Install(ctx, req.Name, req.Version, root, req.Force)
	if err != nil {
		return "", "", err
	}
	if err := m.Discover(); err != nil {
		return name, version, fmt.Errorf("manager: install ok but discover failed: %w", err)
	}
	return name, version, nil
}

// checkRegistryAllowlist enforces Options.TrustedRegistries when set.
// Match is exact (after normalising a trailing slash) — a partial
// match would let an attacker satisfy the check with a subdomain of a
// trusted host. Empty list means the operator has not gated
// registries; the store client's own scheme+host validation still
// applies.
func (m *Manager) checkRegistryAllowlist(url string) error {
	list := m.opts.TrustedRegistries
	if len(list) == 0 {
		return nil
	}
	got := strings.TrimRight(url, "/")
	for _, w := range list {
		if strings.TrimRight(w, "/") == got {
			return nil
		}
	}
	return fmt.Errorf("manager: registry %q is not in TrustedRegistries", url)
}

// Uninstall stops a running plugin, drops its state entry, and removes
// its install directory. The plugin's data dir is left in place —
// removing it would lose secrets and history a re-install probably
// wants to keep. Callers that also want the data dir gone can remove
// it out of band.
func (m *Manager) Uninstall(ctx context.Context, name string) error {
	if err := m.Disable(ctx, name); err != nil {
		// A stopped-but-still-known plugin is fine — Disable is safe on
		// any state. A real error here means the manager could not tear
		// the process down cleanly and we should refuse to remove files
		// from underneath it.
		return fmt.Errorf("manager: uninstall: disable failed: %w", err)
	}
	if err := pack.RemoveInstalled(m.opts.Registry.Root(), name); err != nil {
		return fmt.Errorf("manager: uninstall: %w", err)
	}
	m.mu.Lock()
	delete(m.entries, name)
	m.mu.Unlock()
	m.persistState()
	return nil
}
