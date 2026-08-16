// Package registry discovers installed plugins on disk.
//
// The registry is read-only: it walks a root directory (typically
// /etc/keystone/plugins), parses and validates each plugin.yaml, and hands
// back a list ordered so a dependent comes after its dependencies. Install,
// enable/disable and uninstall are the manager's job — this package only
// reports what the filesystem currently says.
package registry

import (
	"errors"
	"fmt"
	"io/fs"
	"log/slog"
	"os"
	"path/filepath"
	"sort"

	"github.com/kliuchnikovv/keystone/internal/plugin"
)

// Entry is one plugin dir on disk. Manifest is nil when Err is set — a
// broken plugin.yaml is surfaced, not silently dropped, so the manager can
// report it via /plugins.
type Entry struct {
	Dir      string
	Manifest *plugin.Manifest
	Err      error
}

// Name reports the manifest name or, for a broken entry, the dir basename
// (so a plugin whose yaml won't parse can still be addressed by /plugins/...).
func (e Entry) Name() string {
	if e.Manifest != nil {
		return e.Manifest.Metadata.Name
	}
	return filepath.Base(e.Dir)
}

// Registry walks a plugin root.
type Registry struct {
	root string
	log  *slog.Logger
}

// New creates a Registry over root. root does not need to exist yet; Discover
// returns an empty list in that case, which is the right answer for a fresh
// install with no plugins yet.
func New(root string, log *slog.Logger) *Registry {
	if log == nil {
		log = slog.Default()
	}
	return &Registry{root: root, log: log}
}

// Root reports the directory this registry walks.
func (r *Registry) Root() string { return r.root }

// Discover scans root for plugin dirs and returns them ordered so that a
// plugin listing spec.dependsOn always appears after its deps. A cycle
// causes the affected plugins to sort to the end alphabetically and each
// gets Err set — the manager will refuse to enable them.
func (r *Registry) Discover() ([]Entry, error) {
	entries, err := os.ReadDir(r.root)
	if err != nil {
		if errors.Is(err, fs.ErrNotExist) {
			return nil, nil
		}
		return nil, fmt.Errorf("registry: reading %s: %w", r.root, err)
	}

	var out []Entry
	for _, de := range entries {
		if !de.IsDir() {
			continue
		}
		dir := filepath.Join(r.root, de.Name())
		yamlPath := filepath.Join(dir, "plugin.yaml")
		if _, err := os.Stat(yamlPath); errors.Is(err, fs.ErrNotExist) {
			// A subdirectory without plugin.yaml is not ours to complain
			// about. The store might use adjacent dirs (backups, staging)
			// and we should not paint them as broken plugins.
			continue
		}
		mf, perr := plugin.ParseFile(yamlPath)
		e := Entry{Dir: dir, Manifest: mf}
		if perr != nil {
			e.Err = perr
			e.Manifest = nil
		}
		out = append(out, e)
	}

	sortByDeps(out)
	return out, nil
}

// Load reloads one plugin from disk. Used by /plugins/{name}/reload paths.
// Returns fs.ErrNotExist when the dir is gone.
func (r *Registry) Load(name string) (Entry, error) {
	dir := filepath.Join(r.root, name)
	yamlPath := filepath.Join(dir, "plugin.yaml")
	mf, err := plugin.ParseFile(yamlPath)
	if err != nil {
		if errors.Is(err, fs.ErrNotExist) {
			return Entry{}, fs.ErrNotExist
		}
		return Entry{Dir: dir, Err: err}, nil
	}
	return Entry{Dir: dir, Manifest: mf}, nil
}

// sortByDeps orders entries so a plugin appears after the ones it depends
// on. Broken entries (Err != nil) sort last, alphabetically — they have no
// legitimate deps to honour.
func sortByDeps(entries []Entry) {
	// Kahn: build in-degree over the good entries; broken ones bypass the
	// graph and just get appended at the end.
	good := make([]int, 0, len(entries))
	broken := make([]int, 0)
	byName := make(map[string]int)
	for i, e := range entries {
		if e.Err != nil || e.Manifest == nil {
			broken = append(broken, i)
			continue
		}
		byName[e.Manifest.Metadata.Name] = i
		good = append(good, i)
	}

	inDeg := make(map[int]int)
	for _, i := range good {
		inDeg[i] = 0
	}
	edges := make(map[int][]int) // dep -> dependents
	for _, i := range good {
		for _, dep := range entries[i].Manifest.Spec.DependsOn {
			j, ok := byName[dep]
			if !ok {
				// Missing dep: not a hard error at discovery — the manager
				// will refuse to enable this plugin. Skip the edge.
				continue
			}
			edges[j] = append(edges[j], i)
			inDeg[i]++
		}
	}

	ready := make([]int, 0, len(good))
	for _, i := range good {
		if inDeg[i] == 0 {
			ready = append(ready, i)
		}
	}
	sort.SliceStable(ready, func(a, b int) bool {
		return entries[ready[a]].Manifest.Metadata.Name < entries[ready[b]].Manifest.Metadata.Name
	})

	ordered := make([]Entry, 0, len(entries))
	for len(ready) > 0 {
		i := ready[0]
		ready = ready[1:]
		ordered = append(ordered, entries[i])
		next := make([]int, 0)
		for _, j := range edges[i] {
			inDeg[j]--
			if inDeg[j] == 0 {
				next = append(next, j)
			}
		}
		sort.SliceStable(next, func(a, b int) bool {
			return entries[next[a]].Manifest.Metadata.Name < entries[next[b]].Manifest.Metadata.Name
		})
		ready = append(ready, next...)
	}

	// Anything with residual in-degree is in a cycle — append alphabetically.
	remaining := make([]int, 0)
	for i := range inDeg {
		if inDeg[i] > 0 {
			remaining = append(remaining, i)
		}
	}
	sort.SliceStable(remaining, func(a, b int) bool {
		return entries[remaining[a]].Manifest.Metadata.Name < entries[remaining[b]].Manifest.Metadata.Name
	})
	for _, i := range remaining {
		entries[i].Err = fmt.Errorf("registry: %s participates in a spec.dependsOn cycle", entries[i].Manifest.Metadata.Name)
		ordered = append(ordered, entries[i])
	}

	sort.SliceStable(broken, func(a, b int) bool {
		return entries[broken[a]].Name() < entries[broken[b]].Name()
	})
	for _, i := range broken {
		ordered = append(ordered, entries[i])
	}

	copy(entries, ordered)
}
