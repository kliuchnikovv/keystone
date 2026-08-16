package cli

import "net/url"

// ConfigRoot dispatches "keystone config <verb> …". Endpoints hit
// /config; when the daemon lands them, the CLI is ready.

func ConfigRoot(args []string) error {
	if len(args) == 0 {
		return usageErr("config: verb required (get|set|unset|validate)")
	}
	switch args[0] {
	case "get":
		return configGet(args[1:])
	case "set":
		return configSet(args[1:])
	case "unset":
		return configUnset(args[1:])
	case "validate":
		return configValidate(args[1:])
	default:
		return usageErr("config: unknown verb %q", args[0])
	}
}

func configGet(args []string) error {
	if len(args) == 0 {
		var full any
		if err := Get("/config", &full); err != nil {
			return err
		}
		Render(full)
		return nil
	}
	var v any
	if err := Get("/config/"+url.PathEscape(args[0]), &v); err != nil {
		return err
	}
	Render(v)
	return nil
}

func configSet(args []string) error {
	if len(args) < 2 {
		return usageErr("config set <key> <value>")
	}
	// Value is a JSON literal when parseable, else a plain string —
	// same policy as device set-state so both feel the same.
	var v any
	if err := jsonUnmarshalStrict([]byte(args[1]), &v); err != nil {
		v = args[1]
	}
	var resp map[string]any
	if err := Put("/config/"+url.PathEscape(args[0]), map[string]any{"value": v}, &resp); err != nil {
		return err
	}
	Render(resp)
	return nil
}

func configUnset(args []string) error {
	if len(args) == 0 {
		return usageErr("config unset <key>")
	}
	var resp map[string]any
	if err := Delete("/config/"+url.PathEscape(args[0]), &resp); err != nil {
		return err
	}
	Render(resp)
	return nil
}

func configValidate(_ []string) error {
	var resp map[string]any
	if err := Post("/config/validate", nil, &resp); err != nil {
		return err
	}
	Render(resp)
	return nil
}
