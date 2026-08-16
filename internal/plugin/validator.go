package plugin

import (
	"bytes"
	_ "embed"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"sync"

	"github.com/santhosh-tekuri/jsonschema/v6"
	"github.com/santhosh-tekuri/jsonschema/v6/kind"
	"golang.org/x/text/language"
	"golang.org/x/text/message"
	"gopkg.in/yaml.v3"
)

// ManifestFileName is the file a plugin directory is expected to contain.
const ManifestFileName = "plugin.yaml"

//go:embed manifest_schema.json
var schemaJSON []byte

// SchemaJSON returns the normative manifest schema, for tooling that wants to
// publish it (registry CI, editor integrations, `keystone plugin new`).
func SchemaJSON() []byte {
	out := make([]byte, len(schemaJSON))
	copy(out, schemaJSON)
	return out
}

const schemaURL = "https://keystone.io/schemas/plugin-manifest/v1.json"

var (
	compileOnce sync.Once
	compiled    *jsonschema.Schema
	compileErr  error
	printer     = message.NewPrinter(language.English)
)

func manifestSchema() (*jsonschema.Schema, error) {
	compileOnce.Do(func() {
		doc, err := jsonschema.UnmarshalJSON(bytes.NewReader(schemaJSON))
		if err != nil {
			compileErr = fmt.Errorf("embedded manifest schema is not valid JSON: %w", err)
			return
		}
		c := jsonschema.NewCompiler()
		c.AssertFormat()
		if err := c.AddResource(schemaURL, doc); err != nil {
			compileErr = fmt.Errorf("embedded manifest schema is not a valid resource: %w", err)
			return
		}
		compiled, compileErr = c.Compile(schemaURL)
	})
	return compiled, compileErr
}

// ValidationError is one problem found in a manifest, located in the source
// yaml. Line and Column are 1-based; they are zero only when the problem
// cannot be tied to any node (which should not happen for a parsed document).
type ValidationError struct {
	Path    string // dotted instance path, e.g. spec.sidecars[0].restart
	Line    int
	Column  int
	Message string
}

func (e ValidationError) String() string {
	path := e.Path
	if path == "" {
		path = "(root)"
	}
	return fmt.Sprintf("%d:%d: %s: %s", e.Line, e.Column, path, e.Message)
}

// ValidationErrors is the error returned when a manifest does not conform.
// Entries are ordered by position in the file, so output is stable.
type ValidationErrors struct {
	// Source is the file the manifest came from. Empty when it was read from
	// an unnamed reader, in which case messages carry no path prefix.
	Source string
	Errors []ValidationError
}

func (v *ValidationErrors) Error() string {
	var sb strings.Builder
	for i, e := range v.Errors {
		if i > 0 {
			sb.WriteByte('\n')
		}
		if v.Source != "" {
			sb.WriteString(v.Source)
			sb.WriteByte(':')
		}
		sb.WriteString(e.String())
	}
	return sb.String()
}

// Validate reports whether the manifest read from r conforms to plugin
// manifest v1. A non-nil result is either a *ValidationErrors carrying every
// problem found, or a plain error if the input could not be read or parsed as
// yaml at all.
func Validate(r io.Reader) error {
	_, err := parse(r, "")
	return err
}

// ValidateFile is Validate for a path. The path may point at plugin.yaml
// directly or at a plugin directory containing one. Messages are prefixed
// with the file the problem was found in.
func ValidateFile(path string) error {
	_, err := ParseFile(path)
	return err
}

// Parse validates the manifest read from r and returns it decoded.
func Parse(r io.Reader) (*Manifest, error) {
	return parse(r, "")
}

// ParseFile is Parse for a path, accepting either plugin.yaml or the plugin
// directory that holds it.
func ParseFile(path string) (*Manifest, error) {
	if info, err := os.Stat(path); err == nil && info.IsDir() {
		path = filepath.Join(path, ManifestFileName)
	}
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	return parse(bytes.NewReader(data), path)
}

func parse(r io.Reader, source string) (*Manifest, error) {
	data, err := io.ReadAll(r)
	if err != nil {
		return nil, err
	}

	var doc yaml.Node
	if err := yaml.Unmarshal(data, &doc); err != nil {
		// yaml.v3 already names the line in its syntax errors.
		return nil, fmt.Errorf("%s: %w", sourceOr(source, "manifest"), err)
	}
	root := documentRoot(&doc)
	if root == nil {
		return nil, fmt.Errorf("%s: manifest is empty", sourceOr(source, "manifest"))
	}

	instance, err := toJSONValue(root)
	if err != nil {
		return nil, fmt.Errorf("%s: %w", sourceOr(source, "manifest"), err)
	}

	schema, err := manifestSchema()
	if err != nil {
		return nil, err
	}

	if err := schema.Validate(instance); err != nil {
		var verr *jsonschema.ValidationError
		if !errors.As(err, &verr) {
			return nil, err
		}
		return nil, &ValidationErrors{Source: source, Errors: collect(verr, root)}
	}

	var m Manifest
	if err := root.Decode(&m); err != nil {
		return nil, fmt.Errorf("%s: %w", sourceOr(source, "manifest"), err)
	}

	if errs := semanticErrors(&m, root); len(errs) > 0 {
		sortErrors(errs)
		return nil, &ValidationErrors{Source: source, Errors: errs}
	}
	return &m, nil
}

func sourceOr(source, fallback string) string {
	if source != "" {
		return source
	}
	return fallback
}

// documentRoot unwraps the document node yaml.v3 puts on top.
func documentRoot(n *yaml.Node) *yaml.Node {
	if n.Kind == yaml.DocumentNode {
		if len(n.Content) == 0 {
			return nil
		}
		return n.Content[0]
	}
	if n.Kind == 0 {
		return nil
	}
	return n
}

// toJSONValue converts the yaml tree into the plain JSON value shape the
// schema validator expects. Going through encoding/json rather than handing
// over yaml's native types keeps number handling (json.Number) and map keys
// exactly as a JSON document would produce them.
func toJSONValue(root *yaml.Node) (any, error) {
	var raw any
	if err := root.Decode(&raw); err != nil {
		return nil, err
	}
	encoded, err := json.Marshal(raw)
	if err != nil {
		return nil, fmt.Errorf("manifest holds a value that has no JSON equivalent: %w", err)
	}
	return jsonschema.UnmarshalJSON(bytes.NewReader(encoded))
}

// collect flattens the validator's error tree into positioned leaves.
func collect(verr *jsonschema.ValidationError, root *yaml.Node) []ValidationError {
	var out []ValidationError
	seen := make(map[string]bool)

	var walk func(e *jsonschema.ValidationError)
	walk = func(e *jsonschema.ValidationError) {
		loc := e.InstanceLocation
		var node *yaml.Node

		switch k := e.ErrorKind.(type) {
		case *kind.Schema, *kind.Group:
			// Wrappers with no message of their own.
			for _, c := range e.Causes {
				walk(c)
			}
			return
		case *kind.OneOf, *kind.AnyOf:
			// Reporting every failed branch of an alternation buries the real
			// problem. Report the alternation itself and stop descending.
		case *kind.AdditionalProperties:
			// The error hangs off the enclosing object. Name the offending key
			// in the path and point at the key itself, not at its value — the
			// key is the thing the author has to delete or rename.
			if len(k.Properties) > 0 {
				node = keyNode(nodeAt(root, loc), k.Properties[0])
				loc = append(append([]string{}, loc...), k.Properties[0])
			}
		default:
			if len(e.Causes) > 0 {
				for _, c := range e.Causes {
					walk(c)
				}
				return
			}
		}

		if node == nil {
			node = nodeAt(root, loc)
		}
		path := instancePath(loc)
		item := ValidationError{
			Path:    path,
			Line:    node.Line,
			Column:  node.Column,
			Message: describe(e.ErrorKind, path),
		}
		key := item.String()
		if seen[key] {
			return
		}
		seen[key] = true
		out = append(out, item)
	}
	walk(verr)

	sortErrors(out)
	return out
}

// patternHints replaces "does not match pattern ^(0|[1-9][0-9]*)\..." with
// something an author can act on. Keyed by the field the pattern guards rather
// than by the regexp itself, so editing a pattern does not silently drop its
// explanation.
var patternHints = map[string]string{
	"name":            "must be a lowercase slug of 3-40 characters, e.g. my-plugin",
	"version":         "must be a SemVer version, e.g. 1.2.0",
	"keystoneCoreMin": "must be a SemVer version, e.g. 0.5.0",
	"keystoneCoreMax": "must be a SemVer version or a wildcard bound, e.g. 1.x or 1.2.x",
	"capabilities":    "must be a dotted capability id, e.g. transport.matter",
	"dependsOn":       "must be the slug of another plugin, e.g. ha-bridge",
	"tags":            "must be lowercase and hyphen-separated",
	"exec":            "must be a path relative to the plugin directory, e.g. go/my-plugin",
	"homepage":        "must be an http(s) URL",
	"memory":          "must be a size, e.g. 256Mi or 2Gi",
	"disk":            "must be a size, e.g. 256Mi or 2Gi",
	"cpu":             "must be millicores (100m) or cores (0.5, 2)",
}

// describe renders one error kind, preferring a field-specific explanation
// over the validator's generic wording.
func describe(k jsonschema.ErrorKind, path string) string {
	pattern, ok := k.(*kind.Pattern)
	if !ok {
		return k.LocalizedString(printer)
	}
	if hint, ok := patternHints[lastField(path)]; ok {
		return fmt.Sprintf("%q %s", pattern.Got, hint)
	}
	return k.LocalizedString(printer)
}

// lastField returns the field name a path ends in, ignoring array indices:
// spec.capabilities[1] -> capabilities.
func lastField(path string) string {
	if i := strings.LastIndexByte(path, '.'); i >= 0 {
		path = path[i+1:]
	}
	if i := strings.IndexByte(path, '['); i >= 0 {
		path = path[:i]
	}
	return path
}

func sortErrors(errs []ValidationError) {
	sort.SliceStable(errs, func(i, j int) bool {
		a, b := errs[i], errs[j]
		if a.Line != b.Line {
			return a.Line < b.Line
		}
		if a.Column != b.Column {
			return a.Column < b.Column
		}
		if a.Path != b.Path {
			return a.Path < b.Path
		}
		return a.Message < b.Message
	})
}

// nodeAt resolves a JSON-pointer token path against the yaml tree. When a
// token cannot be resolved — a missing required key, an out-of-range index —
// it returns the deepest node it did reach, so the message still points at the
// enclosing block rather than at line 1.
func nodeAt(root *yaml.Node, tokens []string) *yaml.Node {
	cur := root
	for _, token := range tokens {
		next := childOf(cur, token)
		if next == nil {
			return cur
		}
		cur = next
	}
	return cur
}

// keyNode returns the node of the key itself within a mapping, falling back to
// the mapping when the key is not there.
func keyNode(n *yaml.Node, key string) *yaml.Node {
	if n.Kind == yaml.MappingNode {
		for i := 0; i+1 < len(n.Content); i += 2 {
			if n.Content[i].Value == key {
				return n.Content[i]
			}
		}
	}
	return n
}

func childOf(n *yaml.Node, token string) *yaml.Node {
	switch n.Kind {
	case yaml.MappingNode:
		for i := 0; i+1 < len(n.Content); i += 2 {
			if n.Content[i].Value == token {
				return n.Content[i+1]
			}
		}
	case yaml.SequenceNode:
		idx, err := strconv.Atoi(token)
		if err != nil || idx < 0 || idx >= len(n.Content) {
			return nil
		}
		return n.Content[idx]
	}
	return nil
}

// instancePath renders pointer tokens the way a manifest author reads them:
// spec.sidecars[0].restart rather than /spec/sidecars/0/restart.
func instancePath(tokens []string) string {
	var sb strings.Builder
	for _, token := range tokens {
		if _, err := strconv.Atoi(token); err == nil {
			fmt.Fprintf(&sb, "[%s]", token)
			continue
		}
		if sb.Len() > 0 {
			sb.WriteByte('.')
		}
		sb.WriteString(token)
	}
	return sb.String()
}
