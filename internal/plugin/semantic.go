package plugin

import (
	"fmt"
	"slices"
	"strconv"
	"strings"

	"github.com/santhosh-tekuri/jsonschema/v6"
	"gopkg.in/yaml.v3"
)

// semanticErrors holds the rules JSON Schema cannot express, or cannot express
// with an error an author can act on. Each one still reports a yaml position,
// so the two passes are indistinguishable from the outside.
func semanticErrors(m *Manifest, root *yaml.Node) []ValidationError {
	var errs []ValidationError
	at := func(tokens []string, format string, args ...any) {
		node := nodeAt(root, tokens)
		errs = append(errs, ValidationError{
			Path:    instancePath(tokens),
			Line:    node.Line,
			Column:  node.Column,
			Message: fmt.Sprintf(format, args...),
		})
	}

	// A plugin has to do something: run a process, contribute UI, or both.
	if m.Spec.Entrypoint == nil && len(m.UI) == 0 {
		at([]string{"spec"},
			"spec.entrypoint is required unless the plugin declares a top-level \"ui\" block")
	}

	if m.Spec.Entrypoint != nil {
		checkExec(at, []string{"spec", "entrypoint", "exec"}, m.Spec.Entrypoint.Exec)
	}

	seen := make(map[string]int, len(m.Spec.Sidecars))
	for i, sc := range m.Spec.Sidecars {
		idx := strconv.Itoa(i)
		if first, dup := seen[sc.Name]; dup {
			at([]string{"spec", "sidecars", idx, "name"},
				"duplicate sidecar name %q, already declared at spec.sidecars[%d]", sc.Name, first)
		} else {
			seen[sc.Name] = i
		}
		checkExec(at, []string{"spec", "sidecars", idx, "exec"}, sc.Exec)
	}

	for i, dep := range m.Spec.DependsOn {
		if dep == m.Metadata.Name {
			at([]string{"spec", "dependsOn", strconv.Itoa(i)}, "a plugin cannot depend on itself")
		}
	}

	if max := m.Spec.KeystoneCoreMax; max != "" {
		if below(max, m.Spec.KeystoneCoreMin) {
			at([]string{"spec", "keystoneCoreMax"},
				"keystoneCoreMax %q is below keystoneCoreMin %q", max, m.Spec.KeystoneCoreMin)
		}
	}

	if m.Spec.Config != nil {
		if msg := checkEmbeddedSchema(m.Spec.Config.Schema); msg != "" {
			at([]string{"spec", "config", "schema"}, "%s", msg)
		}
	}

	return errs
}

type reporter func(tokens []string, format string, args ...any)

// checkExec rejects paths that escape the plugin directory. The schema's
// pattern already blocks absolute paths, but "." is a legal path character so
// traversal has to be caught segment by segment.
func checkExec(at reporter, tokens []string, exec string) {
	if slices.Contains(strings.Split(exec, "/"), "..") {
		at(tokens, "exec path %q must stay inside the plugin directory", exec)
	}
}

// checkEmbeddedSchema compiles spec.config.schema, which the manifest carries
// as an opaque string. A broken config schema would otherwise only surface
// when the core tries to render the settings form.
func checkEmbeddedSchema(raw string) string {
	doc, err := jsonschema.UnmarshalJSON(strings.NewReader(raw))
	if err != nil {
		return fmt.Sprintf("config schema is not valid JSON: %v", err)
	}
	c := jsonschema.NewCompiler()
	const url = "https://keystone.io/schemas/plugin-config.json"
	if err := c.AddResource(url, doc); err != nil {
		return fmt.Sprintf("config schema is not a valid JSON Schema: %v", err)
	}
	if _, err := c.Compile(url); err != nil {
		return fmt.Sprintf("config schema is not a valid JSON Schema: %v", compactSchemaErr(err))
	}
	return ""
}

// compactSchemaErr picks the one useful line out of the compiler's nested
// report. Its first lines are structural ("'allOf' failed", "'anyOf' failed")
// and its last is usually the least specific branch of an alternation; the
// first line that states an actual expectation is what the author needs.
func compactSchemaErr(err error) string {
	lines := strings.Split(err.Error(), "\n")
	for _, line := range lines[1:] {
		line = strings.TrimSpace(strings.TrimPrefix(strings.TrimSpace(line), "- "))
		if line == "" || strings.HasSuffix(line, "failed") {
			continue
		}
		return line
	}
	return strings.TrimSpace(lines[len(lines)-1])
}

// below reports whether version max sorts before min. Both have already passed
// their schema patterns, so parsing is best-effort: anything unparseable is
// treated as "not below" rather than invented as an error.
func below(max, min string) bool {
	maxParts, ok := versionParts(max)
	if !ok {
		return false
	}
	minParts, ok := versionParts(min)
	if !ok {
		return false
	}
	for i := range maxParts {
		if maxParts[i] != minParts[i] {
			return maxParts[i] < minParts[i]
		}
	}
	return false
}

// versionParts turns 1.2.3, 1.2.x and 1.x into a comparable triple, with
// wildcards standing in for "no upper bound at this level".
func versionParts(v string) ([3]int, bool) {
	var out [3]int
	if i := strings.IndexAny(v, "-+"); i >= 0 {
		v = v[:i]
	}
	fields := strings.Split(v, ".")
	if len(fields) > 3 {
		return out, false
	}
	for i := range out {
		out[i] = int(^uint(0) >> 1) // unspecified or wildcard: unbounded
		if i >= len(fields) || fields[i] == "x" {
			continue
		}
		n, err := strconv.Atoi(fields[i])
		if err != nil {
			return out, false
		}
		out[i] = n
	}
	return out, true
}
