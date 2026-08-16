package dirigera

import (
	"context"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
)

// newTestClient wires a Client to an httptest server. The test server
// serves plain HTTP so we don't need to fabricate certificates; the
// production TLS path is exercised by TestBuildTLSConfig below.
func newTestClient(t *testing.T, handler http.Handler) *Client {
	t.Helper()
	srv := httptest.NewServer(handler)
	t.Cleanup(srv.Close)
	return &Client{
		BaseURL: srv.URL,
		Token:   "tk",
		HTTP:    srv.Client(),
	}
}

func TestListDevices(t *testing.T) {
	c := newTestClient(t, http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path != "/v1/devices" {
			t.Errorf("path = %s", r.URL.Path)
		}
		if got := r.Header.Get("authorization"); got != "Bearer tk" {
			t.Errorf("auth header = %s", got)
		}
		_ = json.NewEncoder(w).Encode([]Device{
			{ID: "d1", Type: "light", DeviceType: "light", Attributes: map[string]any{"isOn": true, "lightLevel": 42.0}},
		})
	}))
	devices, err := c.ListDevices(context.Background())
	if err != nil {
		t.Fatal(err)
	}
	if len(devices) != 1 || devices[0].ID != "d1" {
		t.Fatalf("unexpected: %+v", devices)
	}
}

func TestSetAttributes_PatchesWithBody(t *testing.T) {
	var got []map[string]any
	c := newTestClient(t, http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPatch {
			t.Errorf("method = %s", r.Method)
		}
		if err := json.NewDecoder(r.Body).Decode(&got); err != nil {
			t.Fatal(err)
		}
		w.WriteHeader(http.StatusNoContent)
	}))
	if err := c.SetAttributes(context.Background(), "d1", map[string]any{"isOn": true}); err != nil {
		t.Fatal(err)
	}
	if len(got) != 1 || got[0]["attributes"].(map[string]any)["isOn"] != true {
		t.Fatalf("unexpected body: %+v", got)
	}
}

func TestErrors_UnauthorizedAndNotFound(t *testing.T) {
	cases := []struct {
		status int
		want   error
	}{
		{http.StatusUnauthorized, ErrUnauthorized},
		{http.StatusNotFound, ErrNotFound},
	}
	for _, tc := range cases {
		t.Run(tc.want.Error(), func(t *testing.T) {
			c := newTestClient(t, http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
				w.WriteHeader(tc.status)
			}))
			err := c.doJSON(context.Background(), http.MethodGet, "/x", nil, nil)
			if err != tc.want {
				t.Fatalf("got %v, want %v", err, tc.want)
			}
		})
	}
}

func TestBuildTLSConfig_RequiresTrustAnchor(t *testing.T) {
	if _, err := buildTLSConfig(Config{}); err == nil {
		t.Fatal("empty Config should fail — TLS verification is not optional")
	}
	if _, err := buildTLSConfig(Config{CAPEM: []byte("not-a-pem")}); err == nil {
		t.Fatal("garbage PEM should fail")
	}
	if _, err := buildTLSConfig(Config{PinnedSHA256: "not-hex"}); err == nil {
		t.Fatal("non-hex pin should fail")
	}
	if _, err := buildTLSConfig(Config{PinnedSHA256: strings.Repeat("aa", 32)}); err != nil {
		t.Fatalf("valid pin should build: %v", err)
	}
}
