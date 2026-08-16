package cli

import (
	"encoding/json"
	"flag"
	"net/url"
)

type device struct {
	ID           string         `json:"id"`
	Type         string         `json:"type"`
	Name         string         `json:"name"`
	Manufacturer string         `json:"manufacturer,omitempty"`
	Model        string         `json:"model,omitempty"`
	Transport    string         `json:"transport"`
	TransportRef string         `json:"transport_ref"`
	Features     []deviceFeature `json:"features"`
}

type deviceFeature struct {
	Key string `json:"key"`
}

// DeviceRoot dispatches "keystone device <verb> …".
func DeviceRoot(args []string) error {
	if len(args) == 0 {
		return usageErr("device: verb required (list|get|set-state|invoke|delete)")
	}
	verb, rest := args[0], args[1:]
	switch verb {
	case "list", "ls":
		return deviceList(rest)
	case "get":
		return deviceGet(rest)
	case "set-state":
		return deviceSetState(rest)
	case "invoke":
		return deviceInvoke(rest)
	case "delete", "rm":
		return deviceDelete(rest)
	default:
		return usageErr("device: unknown verb %q", verb)
	}
}

func deviceList(_ []string) error {
	var out struct{ Devices []device `json:"devices"` }
	if err := Get("/devices", &out); err != nil {
		return err
	}
	if G().Output == "json" {
		Render(out.Devices)
		return nil
	}
	rows := make([][]string, 0, len(out.Devices))
	for _, d := range out.Devices {
		feats := ""
		for i, f := range d.Features {
			if i > 0 {
				feats += ","
			}
			feats += f.Key
		}
		rows = append(rows, []string{d.ID, d.Name, d.Type, d.Transport, feats})
	}
	Render(Table{
		Header: []string{"ID", "NAME", "TYPE", "TRANSPORT", "FEATURES"},
		Rows:   rows,
	})
	return nil
}

func deviceGet(args []string) error {
	if len(args) == 0 {
		return usageErr("device get <id>")
	}
	var d device
	if err := Get("/devices/"+url.PathEscape(args[0]), &d); err != nil {
		return err
	}
	Render(d)
	return nil
}

func deviceSetState(args []string) error {
	// keystone device set-state <id> <feature> <key> <value-json>
	if len(args) < 4 {
		return usageErr("device set-state <id> <feature> <key> <value-json>")
	}
	var v any
	if err := json.Unmarshal([]byte(args[3]), &v); err != nil {
		// Fall back to raw string — a common ergonomic case: "true"
		// without quotes, "42.5" without quotes.
		v = args[3]
	}
	body := map[string]any{"feature": args[1], "key": args[2], "value": v}
	var resp map[string]any
	if err := Post("/devices/"+url.PathEscape(args[0])+"/state", body, &resp); err != nil {
		return err
	}
	Render(resp)
	return nil
}

func deviceInvoke(args []string) error {
	// keystone device invoke <id> <feature> <action> [--param k=v ...]
	fs := flag.NewFlagSet("invoke", flag.ContinueOnError)
	params := multiFlag(fs, "param", "action parameter as key=value (JSON value)")
	if err := fs.Parse(args); err != nil {
		return err
	}
	rest := fs.Args()
	if len(rest) < 3 {
		return usageErr("device invoke <id> <feature> <action> [--param k=v ...]")
	}
	body := map[string]any{"feature": rest[1], "action": rest[2], "params": paramsMap(*params)}
	var resp map[string]any
	if err := Post("/devices/"+url.PathEscape(rest[0])+"/actions", body, &resp); err != nil {
		return err
	}
	Render(resp)
	return nil
}

func deviceDelete(args []string) error {
	if len(args) == 0 {
		return usageErr("device delete <id>")
	}
	var resp map[string]any
	if err := Delete("/devices/"+url.PathEscape(args[0]), &resp); err != nil {
		return err
	}
	Render(resp)
	return nil
}

// paramsMap turns a slice of "k=v" strings into a map, best-effort
// JSON-parsing the value so numbers and booleans arrive on the wire
// as themselves.
func paramsMap(entries []string) map[string]any {
	out := make(map[string]any, len(entries))
	for _, e := range entries {
		k, v, ok := splitOnce(e, "=")
		if !ok {
			continue
		}
		var parsed any
		if err := json.Unmarshal([]byte(v), &parsed); err != nil {
			parsed = v
		}
		out[k] = parsed
	}
	return out
}

func splitOnce(s, sep string) (string, string, bool) {
	for i := 0; i+len(sep) <= len(s); i++ {
		if s[i:i+len(sep)] == sep {
			return s[:i], s[i+len(sep):], true
		}
	}
	return s, "", false
}

// multiFlag registers a --param k=v repeatable flag. Returns a
// pointer to the accumulated slice.
func multiFlag(fs *flag.FlagSet, name, help string) *[]string {
	slice := &[]string{}
	fs.Var((*sliceVar)(slice), name, help)
	return slice
}

type sliceVar []string

func (s *sliceVar) String() string {
	if s == nil {
		return ""
	}
	return "[" + jsonStr(*s) + "]"
}

func (s *sliceVar) Set(v string) error {
	*s = append(*s, v)
	return nil
}

func jsonStr(v any) string {
	b, _ := json.Marshal(v)
	return string(b)
}
