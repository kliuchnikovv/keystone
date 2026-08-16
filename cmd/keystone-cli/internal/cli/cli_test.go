package cli

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"testing"
)

// setup runs each test against a fresh handler-driven server and pins
// the CLI's Global.Host to it. Output stays default (json for tests
// since stdout is not a tty).
func setup(t *testing.T, h http.HandlerFunc) *httptest.Server {
	t.Helper()
	srv := httptest.NewServer(h)
	t.Cleanup(srv.Close)
	SetGlobal(Global{Host: srv.URL, Output: "json"})
	return srv
}

func TestPluginList_MapsHTTPCodesToExitCodes(t *testing.T) {
	setup(t, func(w http.ResponseWriter, _ *http.Request) {
		w.WriteHeader(http.StatusNotFound)
	})
	err := PluginRoot([]string{"list"})
	if err == nil {
		t.Fatal("expected error on 404")
	}
	if got := mapHTTP(err.(*HTTPError).Status); got != CodeNotFound {
		t.Fatalf("code = %v, want %v", got, CodeNotFound)
	}
}

func TestPluginEnable_PostsAndReturnsStatus(t *testing.T) {
	var hits int
	setup(t, func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPost || r.URL.Path != "/plugins/matter/enable" {
			t.Errorf("unexpected %s %s", r.Method, r.URL.Path)
		}
		hits++
		_ = json.NewEncoder(w).Encode(map[string]any{"name": "matter", "state": "running"})
	})
	if err := PluginRoot([]string{"enable", "matter"}); err != nil {
		t.Fatal(err)
	}
	if hits != 1 {
		t.Fatalf("expected 1 hit, got %d", hits)
	}
}

func TestPluginInstall_RequiresRegistryFlag(t *testing.T) {
	SetGlobal(Global{Host: "http://x", Output: "json"})
	err := PluginInstall([]string{"matter"})
	if err == nil {
		t.Fatal("install without --registry must fail")
	}
}

func TestDeviceSetState_ParsesBooleanValue(t *testing.T) {
	var body map[string]any
	setup(t, func(w http.ResponseWriter, r *http.Request) {
		_ = json.NewDecoder(r.Body).Decode(&body)
		_ = json.NewEncoder(w).Encode(map[string]any{"ok": true})
	})
	if err := DeviceRoot([]string{"set-state", "dev1", "onoff", "value", "true"}); err != nil {
		t.Fatal(err)
	}
	if body["value"] != true {
		t.Fatalf("expected boolean true, got %v", body["value"])
	}
}

func TestDeviceInvoke_ParamsMap(t *testing.T) {
	var body map[string]any
	setup(t, func(w http.ResponseWriter, r *http.Request) {
		_ = json.NewDecoder(r.Body).Decode(&body)
		_ = json.NewEncoder(w).Encode(map[string]any{"ok": true})
	})
	err := DeviceRoot([]string{"invoke", "--param", "level=42", "--param", "mode=\"heat\"", "dev1", "thermostat", "set"})
	if err != nil {
		t.Fatal(err)
	}
	params, _ := body["params"].(map[string]any)
	if params["level"] != float64(42) {
		t.Fatalf("level = %v", params["level"])
	}
	if params["mode"] != "heat" {
		t.Fatalf("mode = %v", params["mode"])
	}
}

func TestSystemStatus(t *testing.T) {
	setup(t, func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path != "/health" {
			t.Errorf("path = %s", r.URL.Path)
		}
		_ = json.NewEncoder(w).Encode(map[string]any{"status": "ok"})
	})
	if err := SystemStatus(nil); err != nil {
		t.Fatal(err)
	}
}

func TestMapHTTP(t *testing.T) {
	cases := map[int]Code{
		400: CodeValidation,
		401: CodeAuth,
		403: CodeAuth,
		404: CodeNotFound,
		408: CodeTimeout,
		409: CodeConflict,
		500: CodeError,
	}
	for status, want := range cases {
		if got := mapHTTP(status); got != want {
			t.Errorf("mapHTTP(%d) = %v, want %v", status, got, want)
		}
	}
}
