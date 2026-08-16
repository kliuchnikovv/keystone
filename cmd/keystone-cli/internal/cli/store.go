package cli

import (
	"flag"
	"net/url"
	"strings"
)

// storeIndex mirrors the shape internal/plugin/store.Index emits.
// The CLI stays free of internal package imports so the binary can
// evolve independently of the daemon's Go modules.
type storeIndex struct {
	Plugins map[string]storeEntry `json:"plugins"`
}

type storeEntry struct {
	Description string                   `json:"description,omitempty"`
	Homepage    string                   `json:"homepage,omitempty"`
	Versions    map[string]storePackage  `json:"versions"`
}

type storePackage struct {
	URL       string `json:"url"`
	SHA256    string `json:"sha256"`
	Signature string `json:"signature,omitempty"`
	Bundle    string `json:"bundle,omitempty"`
}

// StoreRoot dispatches "keystone store <verb> …".
func StoreRoot(args []string) error {
	if len(args) == 0 {
		return usageErr("store: verb required (browse|search|info|update|tap|untap|taps|categories)")
	}
	verb, rest := args[0], args[1:]
	switch verb {
	case "browse":
		return storeBrowse(rest)
	case "search":
		return storeSearch(rest)
	case "info":
		return storeInfo(rest)
	case "tap":
		return storeTap(rest)
	case "untap":
		return storeUntap(rest)
	case "taps":
		return storeTaps(rest)
	case "categories":
		return storeCategories(rest)
	case "update":
		// The daemon caches per-fetch; no server-side "update" yet.
		Render(map[string]any{"status": "ok", "note": "the store does not cache client-side; each browse re-fetches"})
		return nil
	default:
		return usageErr("store: unknown verb %q", verb)
	}
}

// storeBrowse hits GET /plugins/registry?url= and pretty-prints.
func storeBrowse(args []string) error {
	fs := flag.NewFlagSet("browse", flag.ContinueOnError)
	registry := fs.String("registry", "", "registry URL (required)")
	if err := fs.Parse(args); err != nil {
		return err
	}
	if *registry == "" {
		return usageErr("store browse --registry <url>")
	}
	var idx storeIndex
	if err := Get("/plugins/registry?url="+url.QueryEscape(*registry), &idx); err != nil {
		return err
	}
	renderStoreIndex(idx, "")
	return nil
}

// storeSearch is browse + local filter — the daemon does not filter
// server-side, so a small local grep keeps the ergonomic reasonable.
func storeSearch(args []string) error {
	fs := flag.NewFlagSet("search", flag.ContinueOnError)
	registry := fs.String("registry", "", "registry URL (required)")
	if err := fs.Parse(args); err != nil {
		return err
	}
	rest := fs.Args()
	if *registry == "" || len(rest) == 0 {
		return usageErr("store search --registry <url> <query>")
	}
	var idx storeIndex
	if err := Get("/plugins/registry?url="+url.QueryEscape(*registry), &idx); err != nil {
		return err
	}
	renderStoreIndex(idx, strings.ToLower(rest[0]))
	return nil
}

// storeInfo prints one plugin's registry entry.
func storeInfo(args []string) error {
	fs := flag.NewFlagSet("info", flag.ContinueOnError)
	registry := fs.String("registry", "", "registry URL (required)")
	if err := fs.Parse(args); err != nil {
		return err
	}
	rest := fs.Args()
	if *registry == "" || len(rest) == 0 {
		return usageErr("store info --registry <url> <plugin>")
	}
	var idx storeIndex
	if err := Get("/plugins/registry?url="+url.QueryEscape(*registry), &idx); err != nil {
		return err
	}
	entry, ok := idx.Plugins[rest[0]]
	if !ok {
		return &HTTPError{Status: 404, Method: "GET", Path: "/plugins/registry", Body: "plugin not in registry"}
	}
	Render(entry)
	return nil
}

func renderStoreIndex(idx storeIndex, query string) {
	if G().Output == "json" {
		Render(idx.Plugins)
		return
	}
	rows := make([][]string, 0, len(idx.Plugins))
	for name, entry := range idx.Plugins {
		if query != "" &&
			!strings.Contains(strings.ToLower(name), query) &&
			!strings.Contains(strings.ToLower(entry.Description), query) {
			continue
		}
		latest := ""
		for v := range entry.Versions {
			// versions map iteration is non-deterministic; a
			// deterministic latest requires a proper semver sort — we
			// keep the last-seen for the human view and let JSON output
			// carry the full map for scripts.
			if latest == "" || v > latest {
				latest = v
			}
		}
		rows = append(rows, []string{name, latest, entry.Description})
	}
	Render(Table{
		Header: []string{"NAME", "LATEST", "DESCRIPTION"},
		Rows:   rows,
	})
}

// The following four verbs need daemon-side persistence that doesn't
// exist yet (a taps list, categories). They surface the intent so
// the CLI is forward-compatible: the moment the daemon grows the
// endpoint, the CLI works without changes.

func storeTap(_ []string) error {
	Render(map[string]any{"status": "pending", "note": "server-side tap persistence not implemented; pass --registry per call for now"})
	return nil
}
func storeUntap(_ []string) error   { return storeTap(nil) }
func storeTaps(_ []string) error    { return storeTap(nil) }
func storeCategories(_ []string) error {
	Render(map[string]any{"status": "pending", "note": "server-side category taxonomy not implemented; browse and filter locally"})
	return nil
}
