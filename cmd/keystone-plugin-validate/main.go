// Command keystone-plugin-validate checks plugin.yaml manifests against
// plugin manifest v1.
//
// It is the CI-facing entry point until the full `keystone plugin validate`
// CLI lands; both are thin wrappers over internal/plugin.
//
//	keystone-plugin-validate ./keystone-plugin-matter
//	keystone-plugin-validate -o json plugins/*/plugin.yaml
//
// Exit codes follow docs/keystone-cli-spec.md §3: 0 ok, 1 generic error,
// 2 validation failure.
package main

import (
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"os"
	"path/filepath"

	"github.com/kliuchnikovv/keystone/internal/plugin"
)

const (
	exitOK         = 0
	exitError      = 1
	exitValidation = 2
)

func main() {
	output := flag.String("o", "text", "output format: text | json")
	flag.Usage = usage
	flag.Parse()

	paths := flag.Args()
	if len(paths) == 0 {
		paths = []string{"."}
	}

	switch *output {
	case "text", "json":
	default:
		fmt.Fprintf(os.Stderr, "unknown output format %q, want text or json\n", *output)
		os.Exit(exitError)
	}

	results := make([]result, 0, len(paths))
	worst := exitOK
	for _, path := range paths {
		r := check(path)
		results = append(results, r)
		if r.code > worst {
			worst = r.code
		}
	}

	if *output == "json" {
		enc := json.NewEncoder(os.Stdout)
		enc.SetIndent("", "  ")
		if err := enc.Encode(results); err != nil {
			fmt.Fprintln(os.Stderr, err)
			os.Exit(exitError)
		}
		os.Exit(worst)
	}

	for _, r := range results {
		r.printText()
	}
	os.Exit(worst)
}

func usage() {
	fmt.Fprintf(os.Stderr, "usage: %s [-o text|json] <plugin.yaml | plugin-dir> ...\n\n", filepath.Base(os.Args[0]))
	flag.PrintDefaults()
}

// result is both the text-mode state and the json-mode document.
type result struct {
	Path   string      `json:"path"`
	Valid  bool        `json:"valid"`
	Error  string      `json:"error,omitempty"`
	Errors []problem   `json:"errors,omitempty"`
	Plugin *pluginInfo `json:"plugin,omitempty"`

	code int
}

type problem struct {
	Line    int    `json:"line"`
	Column  int    `json:"column"`
	Path    string `json:"path"`
	Message string `json:"message"`
}

type pluginInfo struct {
	Name    string `json:"name"`
	Version string `json:"version"`
}

func check(path string) result {
	r := result{Path: path}

	m, err := plugin.ParseFile(path)
	switch {
	case err == nil:
		r.Valid, r.code = true, exitOK
		r.Plugin = &pluginInfo{Name: m.Metadata.Name, Version: m.Metadata.Version}
		return r

	case isValidationFailure(err):
		var verr *plugin.ValidationErrors
		errors.As(err, &verr)
		r.code = exitValidation
		for _, e := range verr.Errors {
			r.Errors = append(r.Errors, problem{
				Line:    e.Line,
				Column:  e.Column,
				Path:    e.Path,
				Message: e.Message,
			})
		}
		return r

	default:
		// Unreadable file, broken yaml, or a manifest that is not a mapping at
		// all — nothing the author can fix field by field.
		r.code, r.Error = exitError, err.Error()
		return r
	}
}

func isValidationFailure(err error) bool {
	var verr *plugin.ValidationErrors
	return errors.As(err, &verr)
}

func (r result) printText() {
	switch {
	case r.Valid:
		fmt.Printf("ok  %s (%s %s)\n", r.Path, r.Plugin.Name, r.Plugin.Version)
	case r.Error != "":
		fmt.Fprintf(os.Stderr, "err %s: %s\n", r.Path, r.Error)
	default:
		fmt.Fprintf(os.Stderr, "err %s: %d problem(s)\n", r.Path, len(r.Errors))
		for _, e := range r.Errors {
			fmt.Fprintf(os.Stderr, "    %s:%d:%d: %s: %s\n", r.Path, e.Line, e.Column, e.Path, e.Message)
		}
	}
}
