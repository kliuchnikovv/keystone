package manager

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"

	"github.com/kliuchnikovv/keystone/internal/plugin"
	"github.com/kliuchnikovv/keystone/internal/plugin/supervisor"
)

// dataDirFor returns the persistent data directory for a plugin. Defaults
// to <registry-root>/../plugin-data/<name> when Options.DataDir is empty,
// so a fresh install with `-plugins-dir ./data/plugins` gets
// `./data/plugin-data/<name>` automatically.
func (m *Manager) dataDirFor(name string) string {
	if m.opts.DataDir != "" {
		return filepath.Join(m.opts.DataDir, name)
	}
	return filepath.Join(filepath.Dir(m.opts.Registry.Root()), "plugin-data", name)
}

// startSidecars brings up every manifest-declared sidecar for a plugin,
// stopping any it already started if a later one fails. Returns the live
// supervised processes so Disable can tear them back down.
func (m *Manager) startSidecars(pluginName, pluginDir string, sidecars []plugin.Sidecar, baseEnv map[string]string) ([]*supervisor.Process, error) {
	out := make([]*supervisor.Process, 0, len(sidecars))
	for _, sc := range sidecars {
		argv, err := sidecarCommand(sc, pluginDir)
		if err != nil {
			stopAll(out)
			return nil, fmt.Errorf("manager: sidecar %q of %q: %w", sc.Name, pluginName, err)
		}
		p, err := supervisor.NewProcess(supervisor.ProcessConfig{
			Label:   pluginName + "/" + sc.Name,
			Exec:    argv,
			WorkDir: pluginDir,
			Env:     sidecarEnv(sc.Env, baseEnv),
			Restart: mapSidecarRestart(sc.RestartPolicy()),
			Logger:  m.log,
			OnStdout: func(line string) {
				if m.opts.OnPluginLog != nil {
					m.opts.OnPluginLog(pluginName, "stdout:"+sc.Name, line)
				}
			},
			OnStderr: func(line string) {
				if m.opts.OnPluginLog != nil {
					m.opts.OnPluginLog(pluginName, "stderr:"+sc.Name, line)
				}
			},
		})
		if err != nil {
			stopAll(out)
			return nil, err
		}
		if err := p.Start(); err != nil {
			stopAll(out)
			return nil, err
		}
		out = append(out, p)
	}
	return out, nil
}

// sidecarCommand translates a manifest sidecar declaration into argv.
// The runtime field selects the interpreter — node20 / node22 -> node,
// python3.11 -> python3, static / empty -> exec directly. Anything else
// is treated as static so an operator can pin an unusual interpreter
// on PATH.
func sidecarCommand(sc plugin.Sidecar, pluginDir string) ([]string, error) {
	execPath := sc.Exec
	if !filepath.IsAbs(execPath) {
		execPath = filepath.Join(pluginDir, execPath)
	}
	runtime := strings.ToLower(string(sc.Runtime))
	switch {
	case runtime == "" || runtime == "static":
		return append([]string{execPath}, sc.Args...), nil
	case strings.HasPrefix(runtime, "node"):
		return append([]string{"node", execPath}, sc.Args...), nil
	case strings.HasPrefix(runtime, "python"):
		return append([]string{"python3", execPath}, sc.Args...), nil
	default:
		return append([]string{execPath}, sc.Args...), nil
	}
}

// sidecarEnv builds the env for one declared sidecar: OS env plus the
// base plugin env (name, data dir) plus the sidecar's own entries, with
// manifest values expanded so $KEYSTONE_PLUGIN_DATA works inline.
func sidecarEnv(scEnv, base map[string]string) []string {
	// Union starts from the base so a sidecar can override a base key on
	// purpose. Every value passes through Expand so manifest-level
	// references land as concrete paths.
	merged := make(map[string]string, len(base)+len(scEnv))
	for k, v := range base {
		merged[k] = v
	}
	for k, v := range scEnv {
		merged[k] = os.Expand(v, func(name string) string {
			if val, ok := base[name]; ok {
				return val
			}
			return os.Getenv(name)
		})
	}
	out := make([]string, 0, len(merged))
	for k, v := range merged {
		out = append(out, k+"="+v)
	}
	return out
}

// entrypointEnv builds the env for the entrypoint: base plugin env plus
// the manifest's entrypoint.env entries, expanded the same way. Kept
// separate from sidecarEnv so a change to one does not silently touch
// the other.
func entrypointEnv(epEnv map[string]string, base map[string]string) []string {
	merged := make(map[string]string, len(base)+len(epEnv))
	for k, v := range base {
		merged[k] = v
	}
	for k, v := range epEnv {
		merged[k] = os.Expand(v, func(name string) string {
			if val, ok := base[name]; ok {
				return val
			}
			return os.Getenv(name)
		})
	}
	out := make([]string, 0, len(merged))
	for k, v := range merged {
		out = append(out, k+"="+v)
	}
	return out
}

// mapSidecarRestart translates a manifest Restart value onto the
// supervisor policy. Values line up 1:1 by design.
func mapSidecarRestart(r plugin.Restart) supervisor.RestartPolicy {
	switch r {
	case plugin.RestartAlways:
		return supervisor.RestartAlways
	case plugin.RestartNever:
		return supervisor.RestartNever
	default:
		return supervisor.RestartOnFailure
	}
}

// stopAll tears every sidecar down in reverse order. Errors are swallowed
// — a sidecar that refuses to stop is logged upstream by its own supervisor,
// and blocking Disable on a stuck one would strand every downstream caller.
func stopAll(procs []*supervisor.Process) {
	for i := len(procs) - 1; i >= 0; i-- {
		_ = procs[i].Stop()
	}
}
