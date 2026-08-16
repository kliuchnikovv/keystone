package plugins_test

import (
	"context"
	"encoding/json"
	"errors"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"

	pluginmf "github.com/kliuchnikovv/keystone/internal/plugin"
	"github.com/kliuchnikovv/keystone/internal/api/plugins"
	"github.com/kliuchnikovv/keystone/internal/plugin/manager"
)

type stubManager struct {
	items       map[string]manager.PluginStatus
	enableErr   error
	disableErr  error
	discoverErr error
	enabled     []string
	disabled    []string
}

func (s *stubManager) List() []manager.PluginStatus {
	out := make([]manager.PluginStatus, 0, len(s.items))
	for _, v := range s.items {
		out = append(out, v)
	}
	return out
}

func (s *stubManager) Get(name string) (manager.PluginStatus, bool) {
	v, ok := s.items[name]
	return v, ok
}

func (s *stubManager) Enable(_ context.Context, name string) error {
	if s.enableErr != nil {
		return s.enableErr
	}
	s.enabled = append(s.enabled, name)
	st := s.items[name]
	st.State = manager.StateRunning
	st.Connected = true
	s.items[name] = st
	return nil
}

func (s *stubManager) Disable(_ context.Context, name string) error {
	if s.disableErr != nil {
		return s.disableErr
	}
	s.disabled = append(s.disabled, name)
	st := s.items[name]
	st.State = manager.StateStopped
	st.Connected = false
	s.items[name] = st
	return nil
}

func (s *stubManager) Discover() error { return s.discoverErr }

func newServer(mgr plugins.Manager) *httptest.Server {
	mux := http.NewServeMux()
	plugins.Register(mux, mgr)
	return httptest.NewServer(mux)
}

func TestList(t *testing.T) {
	stub := &stubManager{items: map[string]manager.PluginStatus{
		"matter": {Name: "matter", State: manager.StateDiscovered, Manifest: &pluginmf.Manifest{}},
	}}
	srv := newServer(stub)
	defer srv.Close()

	resp, err := http.Get(srv.URL + "/plugins")
	if err != nil {
		t.Fatal(err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != 200 {
		t.Fatalf("status %d", resp.StatusCode)
	}
	var body struct {
		Plugins []manager.PluginStatus `json:"plugins"`
	}
	_ = json.NewDecoder(resp.Body).Decode(&body)
	if len(body.Plugins) != 1 || body.Plugins[0].Name != "matter" {
		t.Fatalf("unexpected body: %+v", body)
	}
}

func TestGet_NotFound(t *testing.T) {
	stub := &stubManager{items: map[string]manager.PluginStatus{}}
	srv := newServer(stub)
	defer srv.Close()

	resp, err := http.Get(srv.URL + "/plugins/ghost")
	if err != nil {
		t.Fatal(err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusNotFound {
		t.Fatalf("want 404, got %d", resp.StatusCode)
	}
}

func TestEnableDisable(t *testing.T) {
	stub := &stubManager{items: map[string]manager.PluginStatus{
		"matter": {Name: "matter", State: manager.StateDiscovered},
	}}
	srv := newServer(stub)
	defer srv.Close()

	resp, err := http.Post(srv.URL+"/plugins/matter/enable", "", nil)
	if err != nil {
		t.Fatal(err)
	}
	resp.Body.Close()
	if resp.StatusCode != 200 {
		t.Fatalf("enable: status %d", resp.StatusCode)
	}
	if len(stub.enabled) != 1 || stub.enabled[0] != "matter" {
		t.Fatalf("enable not called: %+v", stub.enabled)
	}
	if stub.items["matter"].State != manager.StateRunning {
		t.Fatalf("state not updated after enable")
	}

	resp, err = http.Post(srv.URL+"/plugins/matter/disable", "", nil)
	if err != nil {
		t.Fatal(err)
	}
	resp.Body.Close()
	if resp.StatusCode != 200 {
		t.Fatalf("disable: status %d", resp.StatusCode)
	}
	if len(stub.disabled) != 1 {
		t.Fatalf("disable not called")
	}
}

func TestEnable_ErrorFromManager(t *testing.T) {
	stub := &stubManager{
		items:     map[string]manager.PluginStatus{"matter": {Name: "matter"}},
		enableErr: errors.New("boom"),
	}
	srv := newServer(stub)
	defer srv.Close()

	resp, err := http.Post(srv.URL+"/plugins/matter/enable", "", nil)
	if err != nil {
		t.Fatal(err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusInternalServerError {
		t.Fatalf("want 500, got %d", resp.StatusCode)
	}
	buf := make([]byte, 256)
	n, _ := resp.Body.Read(buf)
	if !strings.Contains(string(buf[:n]), "boom") {
		t.Fatalf("body missing error: %s", buf[:n])
	}
}

func TestDiscover(t *testing.T) {
	stub := &stubManager{items: map[string]manager.PluginStatus{}}
	srv := newServer(stub)
	defer srv.Close()

	resp, err := http.Post(srv.URL+"/plugins/discover", "", nil)
	if err != nil {
		t.Fatal(err)
	}
	resp.Body.Close()
	if resp.StatusCode != 200 {
		t.Fatalf("status %d", resp.StatusCode)
	}
}
