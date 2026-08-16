package main

import (
	"bytes"
	"embed"
	"flag"
	"fmt"
	"io/fs"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"text/template"
)

//go:embed templates/new
var newFS embed.FS

// scaffoldParams is what every template file sees. Kept small on purpose
// — a scaffold that asks for twenty answers is a scaffold nobody uses.
type scaffoldParams struct {
	Name        string // slug used in package, dir, manifest.name
	DisplayName string // pretty-printed metadata.displayName
	Transport   string // slug for capabilities: transport.<slug>
	Module      string // Go module path, e.g. github.com/you/plugin-foo
	Description string // metadata.description
}

var validSlug = regexp.MustCompile(`^[a-z][a-z0-9-]{0,30}$`)

func runNew(args []string) error {
	fs := flag.NewFlagSet("new", flag.ContinueOnError)
	name := fs.String("name", "", "plugin slug (lowercase, hyphens ok) — required")
	transport := fs.String("transport", "", "transport slug for spec.capabilities: transport.<slug> (defaults to name)")
	module := fs.String("module", "", "Go module path (defaults to github.com/USER/plugin-<name>)")
	display := fs.String("display-name", "", "human-friendly name (defaults to Title Case of name)")
	dir := fs.String("dir", ".", "parent directory the plugin folder is created under")
	description := fs.String("description", "One-line summary shown in the plugin store.", "metadata.description")
	if err := fs.Parse(args); err != nil {
		return err
	}
	if *name == "" {
		return fmt.Errorf("--name is required — e.g. --name my-plugin")
	}
	if !validSlug.MatchString(*name) {
		return fmt.Errorf("--name must be lowercase, start with a letter, and contain only letters, digits and hyphens; got %q", *name)
	}
	if *transport == "" {
		*transport = *name
	}
	if *module == "" {
		*module = "github.com/USER/plugin-" + *name
	}
	if *display == "" {
		*display = titleCase(*name)
	}

	target := filepath.Join(*dir, *name)
	if _, err := os.Stat(target); err == nil {
		return fmt.Errorf("target %s already exists; refusing to overwrite", target)
	}

	params := scaffoldParams{
		Name:        *name,
		DisplayName: *display,
		Transport:   *transport,
		Module:      *module,
		Description: *description,
	}
	if err := renderTree(newFS, "templates/new", target, params); err != nil {
		return err
	}
	fmt.Println("scaffolded", target)
	fmt.Println("next:")
	fmt.Println("  cd", *name)
	fmt.Println("  go mod tidy")
	fmt.Println("  keystone-plugin build")
	return nil
}

// renderTree walks a set of embedded template files and materialises
// each one at destDir, running it through text/template with params.
// Files whose name ends in ".tmpl" have the suffix stripped so the
// scaffolded tree does not carry template extensions around.
func renderTree(source embed.FS, srcRoot, destDir string, params scaffoldParams) error {
	return fs.WalkDir(source, srcRoot, func(path string, d fs.DirEntry, err error) error {
		if err != nil {
			return err
		}
		if d.IsDir() {
			return nil
		}
		raw, err := source.ReadFile(path)
		if err != nil {
			return err
		}
		rel := strings.TrimPrefix(path, srcRoot+"/")
		rel = strings.TrimSuffix(rel, ".tmpl")
		// The scaffold uses "name" as a literal directory placeholder
		// (e.g. internal/name/adapter.go.tmpl → internal/<slug>/adapter.go)
		// because embed does not allow computed paths. Rewrite once here
		// rather than per file.
		rel = strings.ReplaceAll(rel, "/name/", "/"+params.Name+"/")
		out := filepath.Join(destDir, rel)
		if err := os.MkdirAll(filepath.Dir(out), 0o755); err != nil {
			return err
		}
		tpl, err := template.New(rel).Parse(string(raw))
		if err != nil {
			return fmt.Errorf("template %s: %w", path, err)
		}
		var buf bytes.Buffer
		if err := tpl.Execute(&buf, params); err != nil {
			return fmt.Errorf("template %s: %w", path, err)
		}
		return os.WriteFile(out, buf.Bytes(), 0o644)
	})
}

func titleCase(slug string) string {
	parts := strings.Split(slug, "-")
	for i, p := range parts {
		if p == "" {
			continue
		}
		parts[i] = strings.ToUpper(p[:1]) + p[1:]
	}
	return strings.Join(parts, " ")
}
