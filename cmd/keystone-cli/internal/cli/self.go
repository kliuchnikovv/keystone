package cli

// Version is baked into the CLI binary. Bumped by hand on releases.
const Version = "0.1.0"

// SelfRoot dispatches "keystone self <verb> …".
func SelfRoot(args []string) error {
	if len(args) == 0 || args[0] == "version" {
		Render(map[string]string{"version": Version})
		return nil
	}
	return usageErr("self: unknown verb %q", args[0])
}
