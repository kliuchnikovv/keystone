package cli

import (
	"encoding/json"
	"fmt"
	"os"
	"strings"
	"text/tabwriter"
)

// Render prints v in the configured output format. Called from every
// command. In "human" mode a caller can pass a []Row for a tabular
// view; anything else falls back to JSON. In "json" mode v is
// json-marshalled with two-space indent and always ends with a
// trailing newline so pipes are clean.
func Render(v any) {
	if current.Output == "json" {
		enc := json.NewEncoder(os.Stdout)
		enc.SetIndent("", "  ")
		_ = enc.Encode(v)
		return
	}
	switch x := v.(type) {
	case string:
		fmt.Println(x)
	case Table:
		renderTable(x)
	default:
		enc := json.NewEncoder(os.Stdout)
		enc.SetIndent("", "  ")
		_ = enc.Encode(v)
	}
}

// Table is a human-readable table. Header names align with columns
// in each row; empty cells render as "-".
type Table struct {
	Header []string
	Rows   [][]string
}

func renderTable(t Table) {
	w := tabwriter.NewWriter(os.Stdout, 0, 2, 2, ' ', 0)
	if len(t.Header) > 0 {
		fmt.Fprintln(w, strings.Join(t.Header, "\t"))
	}
	for _, row := range t.Rows {
		for i, cell := range row {
			if strings.TrimSpace(cell) == "" {
				row[i] = "-"
			}
		}
		fmt.Fprintln(w, strings.Join(row, "\t"))
	}
	_ = w.Flush()
}

// KV renders a simple key/value list — used by info commands.
func KV(pairs [][2]string) {
	if current.Output == "json" {
		m := map[string]string{}
		for _, kv := range pairs {
			m[kv[0]] = kv[1]
		}
		Render(m)
		return
	}
	w := tabwriter.NewWriter(os.Stdout, 0, 2, 2, ' ', 0)
	for _, kv := range pairs {
		fmt.Fprintf(w, "%s:\t%s\n", kv[0], kv[1])
	}
	_ = w.Flush()
}
