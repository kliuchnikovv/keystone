// Package plugin owns the plugin.yaml contract: the Go model of a manifest,
// its normative JSON Schema, and a validator whose errors point at the exact
// line of the offending yaml. Everything downstream — plugin-manager, the
// /plugins/* API, the git registry's CI, the SDK scaffolder — reads a plugin
// through this package and nowhere else.
//
// The manifest format is specified in docs/plugin-store-architecture.md §3.1.
package plugin

import (
	"fmt"

	"gopkg.in/yaml.v3"
)

// APIVersion is the only apiVersion this package understands.
const APIVersion = "keystone.plugin/v1"

// Kind is the only kind this package understands.
const Kind = "Plugin"

// Manifest is a parsed, validated plugin.yaml.
type Manifest struct {
	APIVersion string   `yaml:"apiVersion"`
	Kind       string   `yaml:"kind"`
	Metadata   Metadata `yaml:"metadata"`
	Spec       Spec     `yaml:"spec"`

	// UI is left untyped on purpose: the ui: block is specified by
	// docs/plugin-ui-integration.md and owned by the [UI] tickets. The
	// validator accepts it as an opaque object so UI-only plugins parse today.
	UI map[string]any `yaml:"ui,omitempty"`
}

// Metadata is the store-facing identity of a plugin.
type Metadata struct {
	Name        string   `yaml:"name"`
	DisplayName string   `yaml:"displayName"`
	Version     string   `yaml:"version"`
	Description string   `yaml:"description"`
	Maintainer  string   `yaml:"maintainer"`
	License     string   `yaml:"license"`
	Homepage    string   `yaml:"homepage,omitempty"`
	Category    Category `yaml:"category"`
	Tags        []string `yaml:"tags,omitempty"`
	TrustTier   Trust    `yaml:"trustTier"`
}

// Category buckets a plugin in the store front page.
type Category string

const (
	CategoryTransport  Category = "transport"
	CategoryDevice     Category = "device"
	CategoryAutomation Category = "automation"
	CategoryAI         Category = "ai"
	CategoryUI         Category = "ui"
	CategoryUtility    Category = "utility"
)

// Trust is the trust tier badge described in §3.2.
type Trust string

const (
	TrustCore         Trust = "core"
	TrustVerified     Trust = "verified"
	TrustExperimental Trust = "experimental"
	TrustUnsafe       Trust = "unsafe"
)

// Spec is everything plugin-manager needs at runtime.
type Spec struct {
	KeystoneCoreMin string `yaml:"keystoneCoreMin"`
	KeystoneCoreMax string `yaml:"keystoneCoreMax,omitempty"`

	// Capabilities are dotted ids identical to the ones a plugin reports in
	// the sidecar v1 hello handshake ("transport.matter",
	// "commission.setup-code"). Keeping both sides in one vocabulary lets the
	// manager cross-check the manifest against the live plugin by set compare.
	Capabilities []string `yaml:"capabilities"`

	DependsOn   []string     `yaml:"dependsOn,omitempty"`
	Permissions []Permission `yaml:"permissions,omitempty"`
	Resources   *Resources   `yaml:"resources,omitempty"`
	Entrypoint  *Entrypoint  `yaml:"entrypoint,omitempty"`
	Sidecars    []Sidecar    `yaml:"sidecars,omitempty"`
	Config      *ConfigSpec  `yaml:"config,omitempty"`
}

// Permission is one entry of spec.permissions. The manifest allows two
// spellings — a bare id ("network.lan") and a single-key mapping carrying
// scopes ("secretstore.read: [matter.fabric-key]") — and both land here.
type Permission struct {
	Name   string
	Scopes []string
}

// UnmarshalYAML collapses both permission spellings into one shape.
func (p *Permission) UnmarshalYAML(node *yaml.Node) error {
	switch node.Kind {
	case yaml.ScalarNode:
		p.Name, p.Scopes = node.Value, nil
		return nil
	case yaml.MappingNode:
		if len(node.Content) != 2 {
			return fmt.Errorf("line %d: a scoped permission must hold exactly one key", node.Line)
		}
		p.Name = node.Content[0].Value
		return node.Content[1].Decode(&p.Scopes)
	default:
		return fmt.Errorf("line %d: permission must be a string or a single-key mapping", node.Line)
	}
}

// MarshalYAML re-emits the spelling the entry came in as.
func (p Permission) MarshalYAML() (any, error) {
	if len(p.Scopes) == 0 {
		return p.Name, nil
	}
	return map[string][]string{p.Name: p.Scopes}, nil
}

// Resources is the k8s-flavoured footprint declaration used for sandboxing.
type Resources struct {
	Memory string `yaml:"memory,omitempty"`
	CPU    string `yaml:"cpu,omitempty"`
	Disk   string `yaml:"disk,omitempty"`
}

// Entrypoint is the plugin's own process. Absent for UI-only plugins.
type Entrypoint struct {
	Exec string            `yaml:"exec"`
	Args []string          `yaml:"args,omitempty"`
	Env  map[string]string `yaml:"env,omitempty"`
}

// Sidecar is a child process the plugin owns and the manager supervises.
type Sidecar struct {
	Name    string            `yaml:"name"`
	Exec    string            `yaml:"exec"`
	Runtime Runtime           `yaml:"runtime"`
	Args    []string          `yaml:"args,omitempty"`
	Env     map[string]string `yaml:"env,omitempty"`
	Restart Restart           `yaml:"restart,omitempty"`
}

// RestartPolicy returns the effective policy, applying the schema default.
func (s Sidecar) RestartPolicy() Restart {
	if s.Restart == "" {
		return RestartOnFailure
	}
	return s.Restart
}

// Runtime names the interpreter a sidecar needs shipped with it.
type Runtime string

const (
	RuntimeNative    Runtime = "native"
	RuntimeNode20    Runtime = "node20"
	RuntimeNode22    Runtime = "node22"
	RuntimePython311 Runtime = "python311"
	RuntimePython312 Runtime = "python312"
)

// Restart is the supervisor policy from §5.
type Restart string

const (
	RestartAlways    Restart = "always"
	RestartOnFailure Restart = "on-failure"
	RestartNever     Restart = "never"
)

// ConfigSpec carries the user-settings schema the core renders a form from.
type ConfigSpec struct {
	// Schema is a JSON Schema document embedded as a string.
	Schema string `yaml:"schema"`
}
