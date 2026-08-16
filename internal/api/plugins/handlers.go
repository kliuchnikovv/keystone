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
	"net/http"

	"github.com/kliuchnikovv/keystone/internal/plugin/manager"
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
}

// Register attaches the /plugins/* routes to mux.
func Register(mux *http.ServeMux, mgr Manager) {
	mux.HandleFunc("GET /plugins", listHandler(mgr))
	mux.HandleFunc("POST /plugins/discover", discoverHandler(mgr))
	mux.HandleFunc("GET /plugins/{name}", getHandler(mgr))
	mux.HandleFunc("POST /plugins/{name}/enable", enableHandler(mgr))
	mux.HandleFunc("POST /plugins/{name}/disable", disableHandler(mgr))
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
