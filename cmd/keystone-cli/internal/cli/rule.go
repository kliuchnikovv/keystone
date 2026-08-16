package cli

import (
	"flag"
	"fmt"
	"io"
	"net/url"
	"os"
	"strings"
	"time"
)

type rule struct {
	ID        string         `json:"id"`
	Name      string         `json:"name"`
	Enabled   bool           `json:"enabled"`
	When      []any          `json:"when,omitempty"`
	Then      []any          `json:"then,omitempty"`
	Raw       map[string]any `json:"raw,omitempty"`
	LastFired string         `json:"last_fired,omitempty"`
}

// RuleRoot dispatches "keystone rule <verb> …".
func RuleRoot(args []string) error {
	if len(args) == 0 {
		return usageErr("rule: verb required (list|get|create|delete|enable|disable|run|history|validate)")
	}
	verb, rest := args[0], args[1:]
	switch verb {
	case "list", "ls":
		return ruleList(rest)
	case "get", "info":
		return ruleGet(rest)
	case "create", "add":
		return ruleCreate(rest)
	case "delete", "rm":
		return ruleDelete(rest)
	case "enable":
		return ruleSetEnabled(rest, true)
	case "disable":
		return ruleSetEnabled(rest, false)
	case "history":
		return ruleHistory(rest)
	case "validate":
		return ruleValidate(rest)
	default:
		return usageErr("rule: unknown verb %q", verb)
	}
}

func ruleList(_ []string) error {
	var out struct{ Rules []rule `json:"rules"` }
	if err := Get("/rules", &out); err != nil {
		return err
	}
	if G().Output == "json" {
		Render(out.Rules)
		return nil
	}
	rows := make([][]string, 0, len(out.Rules))
	for _, r := range out.Rules {
		state := "disabled"
		if r.Enabled {
			state = "enabled"
		}
		rows = append(rows, []string{r.ID, r.Name, state, r.LastFired})
	}
	Render(Table{
		Header: []string{"ID", "NAME", "STATE", "LAST FIRED"},
		Rows:   rows,
	})
	return nil
}

func ruleGet(args []string) error {
	if len(args) == 0 {
		return usageErr("rule get <id>")
	}
	// The daemon exposes rules as a bulk list; there is no
	// /rules/{id} yet, so we fetch the list and filter. If a caller
	// asks by prefix ("d4e5"), pick the first match — matches the
	// git-shorthand ergonomic.
	var out struct{ Rules []rule `json:"rules"` }
	if err := Get("/rules", &out); err != nil {
		return err
	}
	for _, r := range out.Rules {
		if r.ID == args[0] || strings.HasPrefix(r.ID, args[0]) {
			Render(r)
			return nil
		}
	}
	return &HTTPError{Status: 404, Method: "GET", Path: "/rules", Body: fmt.Sprintf("no rule matches %q", args[0])}
}

func ruleCreate(args []string) error {
	fs := flag.NewFlagSet("rule create", flag.ContinueOnError)
	file := fs.String("file", "", "path to rule YAML/JSON (use - for stdin)")
	if err := fs.Parse(args); err != nil {
		return err
	}
	if *file == "" {
		return usageErr("rule create --file <path>|-")
	}
	body, err := readRuleBody(*file)
	if err != nil {
		return err
	}
	var created rule
	if err := Post("/rules", body, &created); err != nil {
		return err
	}
	Render(created)
	return nil
}

func ruleDelete(args []string) error {
	if len(args) == 0 {
		return usageErr("rule delete <id>")
	}
	var resp map[string]any
	if err := Delete("/rules/"+url.PathEscape(args[0]), &resp); err != nil {
		return err
	}
	Render(resp)
	return nil
}

// ruleSetEnabled uses the same POST /rules endpoint that upserts —
// enable/disable is a partial update on the existing rule.
func ruleSetEnabled(args []string, enabled bool) error {
	if len(args) == 0 {
		return usageErr("rule enable|disable <id>")
	}
	// Fetch, patch enabled, upsert. Server is single source of truth
	// for the rule body — no local diff.
	var out struct{ Rules []rule `json:"rules"` }
	if err := Get("/rules", &out); err != nil {
		return err
	}
	var target *rule
	for i := range out.Rules {
		if out.Rules[i].ID == args[0] || strings.HasPrefix(out.Rules[i].ID, args[0]) {
			target = &out.Rules[i]
			break
		}
	}
	if target == nil {
		return &HTTPError{Status: 404, Method: "GET", Path: "/rules", Body: fmt.Sprintf("no rule matches %q", args[0])}
	}
	target.Enabled = enabled
	body := target.Raw
	if body == nil {
		body = map[string]any{}
	}
	body["id"] = target.ID
	body["enabled"] = enabled
	var updated rule
	if err := Post("/rules", body, &updated); err != nil {
		return err
	}
	Render(updated)
	return nil
}

func ruleHistory(args []string) error {
	fs := flag.NewFlagSet("rule history", flag.ContinueOnError)
	since := fs.Duration("since", time.Hour, "time window")
	if err := fs.Parse(args); err != nil {
		return err
	}
	rest := fs.Args()
	path := "/rules/runs"
	if len(rest) > 0 {
		path += "?rule=" + url.QueryEscape(rest[0])
	}
	// The daemon returns a raw list; we forward it verbatim and let
	// jq filter downstream on --since.
	_ = since
	var runs []map[string]any
	if err := Get(path, &runs); err != nil {
		return err
	}
	Render(runs)
	return nil
}

func ruleValidate(args []string) error {
	fs := flag.NewFlagSet("rule validate", flag.ContinueOnError)
	file := fs.String("file", "", "path to rule YAML/JSON (use - for stdin)")
	if err := fs.Parse(args); err != nil {
		return err
	}
	if *file == "" {
		return usageErr("rule validate --file <path>|-")
	}
	// A rule endpoint for pure validation does not exist yet; the
	// safest local check is a JSON/YAML round-trip. Real semantic
	// validation waits on server-side --dry-run.
	if _, err := readRuleBody(*file); err != nil {
		return err
	}
	Render(map[string]any{"status": "ok", "note": "syntax-only; semantic validation pending server --dry-run"})
	return nil
}

func readRuleBody(path string) (map[string]any, error) {
	var raw []byte
	var err error
	if path == "-" {
		raw, err = io.ReadAll(os.Stdin)
	} else {
		raw, err = os.ReadFile(path)
	}
	if err != nil {
		return nil, err
	}
	// Accept JSON directly; YAML support would need a dep, and the
	// daemon accepts JSON on /rules so we do too. YAML users convert
	// via `yq . rule.yaml | keystone rule create --file -`.
	var body map[string]any
	if err := jsonUnmarshalStrict(raw, &body); err != nil {
		return nil, fmt.Errorf("rule body must be JSON (pipe yaml through `yq . rule.yaml`): %w", err)
	}
	return body, nil
}
