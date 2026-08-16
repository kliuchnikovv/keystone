// Package plugins exposes /plugins/* as thin HTTP handlers over the plugin
// manager.
//
// Install/uninstall live one layer up in a Store package because they own
// disk state; the manager only speaks to what is already installed. Logs and
// config endpoints will land here once the corresponding manager APIs exist.
package plugins

import (
	"context"
	"encoding/json"
	"errors"
	"io"
	"net/http"
	"strconv"

	"github.com/kliuchnikovv/keystone/internal/plugin/manager"
	"github.com/kliuchnikovv/keystone/internal/plugin/store"
)

// Manager is the surface these handlers need. Kept minimal so tests can
// stub it and so the package does not pull in the whole manager type by
// import time.
type Manager interface {
	List() []manager.PluginStatus
	Get(name string) (manager.PluginStatus, bool)
	Enable(ctx context.Context, name string) error
	Disable(ctx context.Context, name string) error
	Discover() error

	// Logs returns the most recent N log lines the manager has buffered
	// for a plugin. Follow returns a channel that emits new lines until
	// ctx is done; the channel is closed on end.
	Logs(name string, tail int) []manager.LogLine
	Follow(ctx context.Context, name string) (<-chan manager.LogLine, error)

	// Config reads and writes a plugin's persisted configuration, validating
	// PUT bodies against the manifest's spec.config.schema.
	GetConfig(name string) (json.RawMessage, error)
	PutConfig(name string, cfg json.RawMessage) error

	// Install fetches a plugin tarball from a registry, verifies it,
	// and unpacks into the plugins root. Uninstall removes an installed
	// plugin dir after a graceful disable.
	Install(ctx context.Context, req manager.InstallRequest) (string, string, error)
	Uninstall(ctx context.Context, name string) error

	// BrowseRegistry fetches a registry's index so the store UI can
	// list what is available.
	BrowseRegistry(ctx context.Context, url string) (*store.Index, error)
}

// Register attaches the /plugins/* routes to mux.
func Register(mux *http.ServeMux, mgr Manager) {
	mux.HandleFunc("GET /plugins", listHandler(mgr))
	mux.HandleFunc("POST /plugins/discover", discoverHandler(mgr))
	mux.HandleFunc("GET /plugins/{name}", getHandler(mgr))
	mux.HandleFunc("POST /plugins/{name}/enable", enableHandler(mgr))
	mux.HandleFunc("POST /plugins/{name}/disable", disableHandler(mgr))
	mux.HandleFunc("POST /plugins/{name}/restart", restartHandler(mgr))
	mux.HandleFunc("GET /plugins/{name}/logs", logsHandler(mgr))
	mux.HandleFunc("GET /plugins/{name}/config", getConfigHandler(mgr))
	mux.HandleFunc("PUT /plugins/{name}/config", putConfigHandler(mgr))
	mux.HandleFunc("POST /plugins/install", installHandler(mgr))
	mux.HandleFunc("DELETE /plugins/{name}", uninstallHandler(mgr))
	mux.HandleFunc("GET /plugins/registry", browseHandler(mgr))
}

func browseHandler(mgr Manager) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		url := r.URL.Query().Get("url")
		if url == "" {
			writeJSON(w, http.StatusBadRequest, map[string]any{"error": "url query param required"})
			return
		}
		idx, err := mgr.BrowseRegistry(r.Context(), url)
		if err != nil {
			writeJSON(w, http.StatusBadRequest, map[string]any{"error": err.Error()})
			return
		}
		writeJSON(w, http.StatusOK, idx)
	}
}

func installHandler(mgr Manager) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		var body struct {
			Name     string `json:"name"`
			Version  string `json:"version,omitempty"`
			Registry string `json:"registry"`
			Force    bool   `json:"force,omitempty"`
		}
		if err := json.NewDecoder(io.LimitReader(r.Body, 1<<16)).Decode(&body); err != nil {
			writeJSON(w, http.StatusBadRequest, map[string]any{"error": "reading body: " + err.Error()})
			return
		}
		name, version, err := mgr.Install(r.Context(), manager.InstallRequest{
			Name:     body.Name,
			Version:  body.Version,
			Registry: body.Registry,
			Force:    body.Force,
		})
		if err != nil {
			writeJSON(w, http.StatusBadRequest, map[string]any{"error": err.Error()})
			return
		}
		st, _ := mgr.Get(name)
		writeJSON(w, http.StatusOK, map[string]any{"name": name, "version": version, "status": st})
	}
}

func uninstallHandler(mgr Manager) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		name := r.PathValue("name")
		if _, ok := mgr.Get(name); !ok {
			writeJSON(w, http.StatusNotFound, map[string]any{"error": "plugin not found"})
			return
		}
		if err := mgr.Uninstall(r.Context(), name); err != nil {
			writeJSON(w, http.StatusInternalServerError, map[string]any{"error": err.Error()})
			return
		}
		writeJSON(w, http.StatusOK, map[string]any{"status": "ok", "name": name})
	}
}

func listHandler(mgr Manager) http.HandlerFunc {
	return func(w http.ResponseWriter, _ *http.Request) {
		writeJSON(w, http.StatusOK, map[string]any{"plugins": mgr.List()})
	}
}

func getHandler(mgr Manager) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		name := r.PathValue("name")
		st, ok := mgr.Get(name)
		if !ok {
			writeJSON(w, http.StatusNotFound, map[string]any{"error": "plugin not found"})
			return
		}
		writeJSON(w, http.StatusOK, st)
	}
}

func discoverHandler(mgr Manager) http.HandlerFunc {
	return func(w http.ResponseWriter, _ *http.Request) {
		if err := mgr.Discover(); err != nil {
			writeJSON(w, http.StatusInternalServerError, map[string]any{"error": err.Error()})
			return
		}
		writeJSON(w, http.StatusOK, map[string]any{"plugins": mgr.List()})
	}
}

func enableHandler(mgr Manager) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		name := r.PathValue("name")
		if _, ok := mgr.Get(name); !ok {
			writeJSON(w, http.StatusNotFound, map[string]any{"error": "plugin not found"})
			return
		}
		if err := mgr.Enable(r.Context(), name); err != nil {
			status := http.StatusInternalServerError
			// A precondition failure (missing entrypoint, unmet dep, broken
			// manifest) is a 4xx — a caller can fix it. A supervisor spawn
			// failure is closer to a transient system error and stays 5xx,
			// but we don't have that distinction here yet.
			if errors.Is(err, errInvalidState{}) {
				status = http.StatusConflict
			}
			writeJSON(w, status, map[string]any{"error": err.Error()})
			return
		}
		st, _ := mgr.Get(name)
		writeJSON(w, http.StatusOK, st)
	}
}

func disableHandler(mgr Manager) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		name := r.PathValue("name")
		if _, ok := mgr.Get(name); !ok {
			writeJSON(w, http.StatusNotFound, map[string]any{"error": "plugin not found"})
			return
		}
		if err := mgr.Disable(r.Context(), name); err != nil {
			writeJSON(w, http.StatusInternalServerError, map[string]any{"error": err.Error()})
			return
		}
		st, _ := mgr.Get(name)
		writeJSON(w, http.StatusOK, st)
	}
}

func restartHandler(mgr Manager) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		name := r.PathValue("name")
		if _, ok := mgr.Get(name); !ok {
			writeJSON(w, http.StatusNotFound, map[string]any{"error": "plugin not found"})
			return
		}
		// Sequential disable then enable — the manager already refuses
		// to enable a broken plugin, so a failed re-enable surfaces the
		// underlying reason instead of leaving the plugin in a limbo
		// state.
		if err := mgr.Disable(r.Context(), name); err != nil {
			writeJSON(w, http.StatusInternalServerError, map[string]any{"error": err.Error()})
			return
		}
		if err := mgr.Enable(r.Context(), name); err != nil {
			writeJSON(w, http.StatusInternalServerError, map[string]any{"error": err.Error()})
			return
		}
		st, _ := mgr.Get(name)
		writeJSON(w, http.StatusOK, st)
	}
}

func logsHandler(mgr Manager) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		name := r.PathValue("name")
		if _, ok := mgr.Get(name); !ok {
			writeJSON(w, http.StatusNotFound, map[string]any{"error": "plugin not found"})
			return
		}
		tail := 200
		if raw := r.URL.Query().Get("tail"); raw != "" {
			if n, err := strconv.Atoi(raw); err == nil && n > 0 {
				tail = n
			}
		}
		follow := r.URL.Query().Get("follow") == "true"

		if !follow {
			writeJSON(w, http.StatusOK, map[string]any{"lines": mgr.Logs(name, tail)})
			return
		}

		// NDJSON stream: one line per log entry, buffered writer flushed
		// after each line so the client sees output live.
		flusher, ok := w.(http.Flusher)
		if !ok {
			writeJSON(w, http.StatusInternalServerError, map[string]any{"error": "streaming not supported"})
			return
		}
		w.Header().Set("content-type", "application/x-ndjson")
		w.Header().Set("cache-control", "no-cache")
		w.WriteHeader(http.StatusOK)

		// Replay recent lines so a follower joining mid-run has context.
		for _, l := range mgr.Logs(name, tail) {
			_ = json.NewEncoder(w).Encode(l)
		}
		flusher.Flush()

		ch, err := mgr.Follow(r.Context(), name)
		if err != nil {
			return
		}
		for l := range ch {
			if err := json.NewEncoder(w).Encode(l); err != nil {
				return
			}
			flusher.Flush()
		}
	}
}

func getConfigHandler(mgr Manager) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		name := r.PathValue("name")
		cfg, err := mgr.GetConfig(name)
		if err != nil {
			status := http.StatusInternalServerError
			if errors.Is(err, manager.ErrPluginNotFound) {
				status = http.StatusNotFound
			}
			writeJSON(w, status, map[string]any{"error": err.Error()})
			return
		}
		w.Header().Set("content-type", "application/json")
		w.WriteHeader(http.StatusOK)
		_, _ = w.Write(cfg)
	}
}

func putConfigHandler(mgr Manager) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		name := r.PathValue("name")
		body, err := io.ReadAll(io.LimitReader(r.Body, 1<<20))
		if err != nil {
			writeJSON(w, http.StatusBadRequest, map[string]any{"error": "reading body: " + err.Error()})
			return
		}
		if err := mgr.PutConfig(name, body); err != nil {
			// The manager returns wrapped ErrPluginNotFound for unknown
			// names and validation errors otherwise; both are 4xx.
			writeJSON(w, http.StatusBadRequest, map[string]any{"error": err.Error()})
			return
		}
		writeJSON(w, http.StatusOK, map[string]any{"status": "ok"})
	}
}

// errInvalidState is a sentinel placeholder until manager surfaces typed
// errors. Kept private so it only exists to be swapped in later without a
// signature change on Enable.
type errInvalidState struct{}

func (errInvalidState) Error() string { return "invalid state" }

func writeJSON(w http.ResponseWriter, status int, v any) {
	w.Header().Set("content-type", "application/json")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(v)
}
