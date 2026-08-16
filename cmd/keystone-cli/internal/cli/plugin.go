package cli

import (
	"flag"
	"fmt"
)

type pluginStatus struct {
	Name      string `json:"name"`
	Version   string `json:"version"`
	State     string `json:"state"`
	Connected bool   `json:"connected"`
	LastError string `json:"last_error,omitempty"`
}

// PluginRoot dispatches "keystone plugin <verb> …".
func PluginRoot(args []string) error {
	if len(args) == 0 {
		return usageErr("plugin: verb required (list|info|enable|disable|restart|install|uninstall|logs)")
	}
	verb, rest := args[0], args[1:]
	switch verb {
	case "list", "ls":
		return pluginList(rest)
	case "info":
		return pluginInfo(rest)
	case "enable":
		return pluginEnable(rest)
	case "disable":
		return pluginDisable(rest)
	case "restart":
		return pluginRestart(rest)
	case "install":
		return PluginInstall(rest)
	case "uninstall", "remove":
		return PluginUninstall(rest)
	case "logs":
		return pluginLogs(rest)
	default:
		return usageErr("plugin: unknown verb %q", verb)
	}
}

func pluginList(_ []string) error {
	var out struct{ Plugins []pluginStatus `json:"plugins"` }
	if err := Get("/plugins", &out); err != nil {
		return err
	}
	if G().Output == "json" {
		Render(out.Plugins)
		return nil
	}
	rows := make([][]string, 0, len(out.Plugins))
	for _, p := range out.Plugins {
		rows = append(rows, []string{p.Name, p.Version, p.State, boolStr(p.Connected), p.LastError})
	}
	Render(Table{
		Header: []string{"NAME", "VERSION", "STATE", "CONNECTED", "LAST_ERROR"},
		Rows:   rows,
	})
	return nil
}

func pluginInfo(args []string) error {
	if len(args) == 0 {
		return usageErr("plugin info <name>")
	}
	var st pluginStatus
	if err := Get("/plugins/"+args[0], &st); err != nil {
		return err
	}
	KV([][2]string{
		{"name", st.Name},
		{"version", st.Version},
		{"state", st.State},
		{"connected", boolStr(st.Connected)},
		{"last_error", st.LastError},
	})
	return nil
}

func pluginEnable(args []string) error {
	if len(args) == 0 {
		return usageErr("plugin enable <name>")
	}
	var st pluginStatus
	if err := Post("/plugins/"+args[0]+"/enable", nil, &st); err != nil {
		return err
	}
	Render(st)
	return nil
}

func pluginDisable(args []string) error {
	if len(args) == 0 {
		return usageErr("plugin disable <name>")
	}
	var st pluginStatus
	if err := Post("/plugins/"+args[0]+"/disable", nil, &st); err != nil {
		return err
	}
	Render(st)
	return nil
}

func pluginRestart(args []string) error {
	if len(args) == 0 {
		return usageErr("plugin restart <name>")
	}
	var st pluginStatus
	if err := Post("/plugins/"+args[0]+"/restart", nil, &st); err != nil {
		return err
	}
	Render(st)
	return nil
}

// PluginInstall is exposed so the top-level `keystone install` shortcut
// can call it directly.
func PluginInstall(args []string) error {
	fs := flag.NewFlagSet("install", flag.ContinueOnError)
	registry := fs.String("registry", "", "registry URL (required)")
	version := fs.String("version", "latest", "version to install")
	force := fs.Bool("force", false, "overwrite an existing install")
	if err := fs.Parse(args); err != nil {
		return err
	}
	if fs.NArg() == 0 {
		return usageErr("install: plugin name required")
	}
	if *registry == "" {
		return usageErr("install: --registry is required")
	}
	body := map[string]any{
		"name":     fs.Arg(0),
		"version":  *version,
		"registry": *registry,
		"force":    *force,
	}
	var resp map[string]any
	if err := Post("/plugins/install", body, &resp); err != nil {
		return err
	}
	Render(resp)
	return nil
}

// PluginUninstall backs the top-level shortcut and the sub-verb.
func PluginUninstall(args []string) error {
	if len(args) == 0 {
		return usageErr("uninstall <name>")
	}
	var resp map[string]any
	if err := Delete("/plugins/"+args[0], &resp); err != nil {
		return err
	}
	Render(resp)
	return nil
}

func pluginLogs(args []string) error {
	fs := flag.NewFlagSet("logs", flag.ContinueOnError)
	tail := fs.Int("tail", 200, "how many recent lines to show")
	follow := fs.Bool("follow", false, "keep streaming new lines")
	if err := fs.Parse(args); err != nil {
		return err
	}
	if fs.NArg() == 0 {
		return usageErr("plugin logs <name>")
	}
	name := fs.Arg(0)
	path := fmt.Sprintf("/plugins/%s/logs?tail=%d", name, *tail)
	if *follow {
		path += "&follow=true"
		return GetStream(path, func(line []byte) bool {
			fmt.Println(string(line))
			return true
		})
	}
	var out struct{ Lines []map[string]any `json:"lines"` }
	if err := Get(path, &out); err != nil {
		return err
	}
	Render(out.Lines)
	return nil
}

func boolStr(b bool) string {
	if b {
		return "yes"
	}
	return "no"
}

func usageErr(format string, a ...any) error {
	return fmt.Errorf(format, a...)
}
