package dirigera

import (
	"context"
	"encoding/json"
	"log/slog"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"github.com/coder/websocket"

	"github.com/kliuchnikovv/keystone/internal/domain"
	"github.com/kliuchnikovv/keystone/internal/ports"
)

func TestDeriveWSURL(t *testing.T) {
	cases := map[string]string{
		"https://hub.local:8443":  "wss://hub.local:8443/v1",
		"http://hub.local:8443":   "ws://hub.local:8443/v1",
		"https://hub.local:8443/": "wss://hub.local:8443/v1",
	}
	for in, want := range cases {
		got, err := deriveWSURL(in)
		if err != nil {
			t.Fatalf("%s: %v", in, err)
		}
		if got != want {
			t.Errorf("deriveWSURL(%s) = %s, want %s", in, got, want)
		}
	}
}

func TestForwardEvents_MapsStateChanged(t *testing.T) {
	a := &Adapter{
		events: make(chan ports.TransportEvent, 8),
		log:    slog.Default(),
	}

	stream := make(chan Event, 4)
	go a.forwardEvents(context.Background(), stream)

	payload, _ := json.Marshal(DeviceStateChanged{
		ID:         "dev1",
		Attributes: map[string]any{"isOn": true, "lightLevel": 60.0, "unknown": "ignored"},
	})
	stream <- Event{Type: "deviceStateChanged", Data: payload}
	close(stream)

	// The hub pushes attributes as a map; iteration order in Go is
	// non-deterministic, so collect both events and check the set.
	deadline := time.After(2 * time.Second)
	got := map[domain.FeatureKey]any{}
	for i := 0; i < 2; i++ {
		select {
		case ev := <-a.events:
			got[ev.Feature] = ev.Value
		case <-deadline:
			t.Fatalf("timed out after %d events", i)
		}
	}
	if got[domain.FeatureOnOff] != true {
		t.Errorf("on/off = %v", got[domain.FeatureOnOff])
	}
	if got[domain.FeatureBrightness] != 60.0 {
		t.Errorf("brightness = %v", got[domain.FeatureBrightness])
	}
}

func TestWatchEvents_HubPushesEvents(t *testing.T) {
	// Serve a WS endpoint at /v1 that immediately sends one event.
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path != "/v1" {
			http.Error(w, "not found", 404)
			return
		}
		if got := r.Header.Get("authorization"); got != "Bearer tk" {
			http.Error(w, "bad auth", 401)
			return
		}
		conn, err := websocket.Accept(w, r, nil)
		if err != nil {
			return
		}
		defer conn.Close(websocket.StatusNormalClosure, "bye")
		payload, _ := json.Marshal(DeviceStateChanged{
			ID:         "dev1",
			Attributes: map[string]any{"isOn": true},
		})
		msg, _ := json.Marshal(Event{Type: "deviceStateChanged", Data: payload})
		_ = conn.Write(r.Context(), websocket.MessageText, msg)
		<-r.Context().Done()
	}))
	defer srv.Close()

	// The httptest server is http:// — deriveWSURL flips to ws://.
	c := &Client{BaseURL: srv.URL, Token: "tk", HTTP: srv.Client()}
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	ch, err := c.WatchEvents(ctx, slog.Default())
	if err != nil {
		t.Fatalf("WatchEvents: %v", err)
	}
	select {
	case ev := <-ch:
		if ev.Type != "deviceStateChanged" {
			t.Errorf("type = %s", ev.Type)
		}
		if !strings.Contains(string(ev.Data), "dev1") {
			t.Errorf("payload missing id: %s", ev.Data)
		}
	case <-time.After(2 * time.Second):
		t.Fatal("no event delivered")
	}
}
