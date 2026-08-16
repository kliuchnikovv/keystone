package cli

// SystemRoot dispatches "keystone system <verb> …".
func SystemRoot(args []string) error {
	if len(args) == 0 {
		return usageErr("system: verb required (status)")
	}
	switch args[0] {
	case "status":
		return SystemStatus(args[1:])
	default:
		return usageErr("system: unknown verb %q", args[0])
	}
}

// SystemStatus asks the daemon whether it's alive. Exposed so the
// top-level `keystone status` shortcut can call it directly.
func SystemStatus(_ []string) error {
	var resp map[string]any
	if err := Get("/health", &resp); err != nil {
		return err
	}
	KV([][2]string{
		{"host", G().Host},
		{"status", jsonStr(resp["status"])},
	})
	return nil
}
