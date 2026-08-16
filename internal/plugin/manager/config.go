package manager

import (
	"encoding/json"
	"errors"
	"fmt"
	"io/fs"
	"os"
	"path/filepath"
	"strings"

	"github.com/santhosh-tekuri/jsonschema/v6"

	"github.com/kliuchnikovv/keystone/internal/plugin"
)

// configFileName is the file inside a plugin's data dir that stores the
// operator's configuration. Kept as JSON so it round-trips through
// spec.config.schema validation transparently.
const configFileName = "config.json"

// GetConfig reads a plugin's saved configuration. A plugin with no
// config yet returns {} — this is the plugin's "you have never set
// anything" state, distinguishable from an unknown plugin (ErrPluginNotFound).
func (m *Manager) GetConfig(name string) (json.RawMessage, error) {
	m.mu.RLock()
	_, ok := m.entries[name]
	m.mu.RUnlock()
	if !ok {
		return nil, ErrPluginNotFound
	}
	path := filepath.Join(m.dataDirFor(name), configFileName)
	raw, err := os.ReadFile(path)
	if errors.Is(err, fs.ErrNotExist) {
		return json.RawMessage(`{}`), nil
	}
	if err != nil {
		return nil, fmt.Errorf("plugin config: read %s: %w", path, err)
	}
	return json.RawMessage(raw), nil
}

// PutConfig writes a plugin's configuration after validating it against
// the manifest's spec.config.schema. A missing schema on the manifest
// means "no validation" — the plugin owns its own contract.
//
// The config is written atomically so a torn write cannot leave the
// plugin unable to boot.
func (m *Manager) PutConfig(name string, body json.RawMessage) error {
	m.mu.RLock()
	r, ok := m.entries[name]
	m.mu.RUnlock()
	if !ok {
		return ErrPluginNotFound
	}
	if err := validateConfig(r.entry.Manifest, body); err != nil {
		return err
	}
	dir := m.dataDirFor(name)
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return err
	}
	path := filepath.Join(dir, configFileName)
	tmp := path + ".tmp"
	// Pretty-print so an operator inspecting the file gets something
	// readable, without a round-trip through a custom marshaller.
	var pretty any
	if err := json.Unmarshal(body, &pretty); err != nil {
		return fmt.Errorf("plugin config: unparseable body: %w", err)
	}
	out, err := json.MarshalIndent(pretty, "", "  ")
	if err != nil {
		return err
	}
	if err := os.WriteFile(tmp, out, 0o644); err != nil {
		return err
	}
	return os.Rename(tmp, path)
}

// validateConfig checks a body against the manifest's config schema.
// A nil manifest or missing schema block skips validation — a plugin
// that has no schema is opting out.
func validateConfig(mf *plugin.Manifest, body json.RawMessage) error {
	if mf == nil || mf.Spec.Config == nil || mf.Spec.Config.Schema == "" {
		return nil
	}
	schemaDoc, err := jsonschema.UnmarshalJSON(strings.NewReader(mf.Spec.Config.Schema))
	if err != nil {
		return fmt.Errorf("plugin config: broken schema: %w", err)
	}
	compiler := jsonschema.NewCompiler()
	if err := compiler.AddResource("config", schemaDoc); err != nil {
		return fmt.Errorf("plugin config: broken schema: %w", err)
	}
	schema, err := compiler.Compile("config")
	if err != nil {
		return fmt.Errorf("plugin config: broken schema: %w", err)
	}
	var doc any
	if err := json.Unmarshal(body, &doc); err != nil {
		return fmt.Errorf("plugin config: body is not valid JSON: %w", err)
	}
	if err := schema.Validate(doc); err != nil {
		return fmt.Errorf("plugin config: %w", err)
	}
	return nil
}
