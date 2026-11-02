package main

import (
	"context"
	"fmt"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"github.com/coder/websocket"
)

// The rules the relay must hold on a publicly reachable port.
func TestRelayServer(t *testing.T) {
	const secret = "a-very-long-test-secret-value"
	const statusToken = "status-token-123"

	r := newRelay(secret, statusToken, nil, MaxClientsPerHub, MaxHubs)
	r.setHubSecrets(map[string]string{"solo": "the-secrets-of-one-hub-only"})
	srv := httptest.NewServer(r)
	defer srv.Close()
	wsBase := "ws" + strings.TrimPrefix(srv.URL, "http")

	ctx, cancel := context.WithTimeout(context.Background(), 20*time.Second)
	defer cancel()

	t.Run("the health check answers without a token", func(t *testing.T) {
		resp, err := http.Get(srv.URL + "/healthz")
		if err != nil {
			t.Fatal(err)
		}
		defer resp.Body.Close()
		if resp.StatusCode != http.StatusOK {
			t.Fatalf("want 200, got %d", resp.StatusCode)
		}
		buf := make([]byte, 64)
		n, _ := resp.Body.Read(buf)
		if strings.Contains(string(buf[:n]), "home") {
			t.Fatalf("the health check must not say anything about the hubs: %q", string(buf[:n]))
		}
	})

	t.Run("the status page needs the token", func(t *testing.T) {
		resp, err := http.Get(srv.URL + "/")
		if err != nil {
			t.Fatal(err)
		}
		defer resp.Body.Close()
		if resp.StatusCode != http.StatusUnauthorized {
			t.Fatalf("want 401, got %d", resp.StatusCode)
		}
	})

	t.Run("a browser origin cannot open a tunnel", func(t *testing.T) {
		_, _, err := websocket.Dial(ctx, wsBase+"/tunnel/home", &websocket.DialOptions{
			HTTPHeader: http.Header{"Origin": []string{"https://evil.example"}},
		})
		if err == nil {
			t.Fatal("the connection must be refused")
		}
	})

	t.Run("a native client that mirrors the address may connect", func(t *testing.T) {
		// React Native's WebSocket on iOS is SocketRocket: it puts the address
		// it dials into the Origin header. That is the relay's own origin, not a
		// foreign site, so it must not be refused.
		host := strings.TrimPrefix(srv.URL, "http://")
		c, _, err := websocket.Dial(ctx, wsBase+"/client/home", &websocket.DialOptions{
			HTTPHeader: http.Header{"Origin": []string{"http://" + host}},
		})
		if err != nil {
			t.Fatalf("the client must connect: %v", err)
		}
		c.Close(websocket.StatusNormalClosure, "")
	})

	t.Run("an invalid hub id is refused", func(t *testing.T) {
		_, _, err := websocket.Dial(ctx, wsBase+"/tunnel/bad%20id", nil)
		if err == nil {
			t.Fatal("the connection must be refused")
		}
	})

	tunnelCtx, tunnelCancel := context.WithCancel(ctx)
	defer tunnelCancel()
	tunnel, _, err := websocket.Dial(tunnelCtx, wsBase+"/tunnel/home", nil)
	if err != nil {
		t.Fatalf("the tunnel must connect: %v", err)
	}
	defer tunnel.Close(websocket.StatusNormalClosure, "")

	t.Run("the right secret is accepted", func(t *testing.T) {
		if err := tunnel.Write(ctx, websocket.MessageText, []byte("!auth:"+secret)); err != nil {
			t.Fatal(err)
		}
		_, data, err := tunnel.Read(ctx)
		if err != nil {
			t.Fatal(err)
		}
		if string(data) != "!ok" {
			t.Fatalf("want !ok, got %q", data)
		}
	})

	t.Run("the token holder sees the hub id", func(t *testing.T) {
		req, _ := http.NewRequest(http.MethodGet, srv.URL+"/", nil)
		req.Header.Set("Authorization", "Bearer "+statusToken)
		resp, err := http.DefaultClient.Do(req)
		if err != nil {
			t.Fatal(err)
		}
		defer resp.Body.Close()
		buf := make([]byte, 256)
		n, _ := resp.Body.Read(buf)
		if resp.StatusCode != http.StatusOK {
			t.Fatalf("want 200, got %d", resp.StatusCode)
		}
		if !strings.Contains(string(buf[:n]), "home") {
			t.Fatalf("the hub id must be there: %q", string(buf[:n]))
		}
	})

	t.Run("a remote client makes the tunnel open a connection", func(t *testing.T) {
		client, _, err := websocket.Dial(ctx, wsBase+"/client/home", nil)
		if err != nil {
			t.Fatalf("the client must connect: %v", err)
		}
		defer client.Close(websocket.StatusNormalClosure, "")
		_, data, err := tunnel.Read(ctx)
		if err != nil {
			t.Fatal(err)
		}
		if !strings.HasPrefix(string(data), "!open:") {
			t.Fatalf("want !open:<cid>, got %q", data)
		}
	})

	t.Run("a frame sent before the hub is ready still reaches the tunnel", func(t *testing.T) {
		client, _, err := websocket.Dial(ctx, wsBase+"/client/home", nil)
		if err != nil {
			t.Fatalf("the client must connect: %v", err)
		}
		defer client.Close(websocket.StatusNormalClosure, "")
		// The client speaks before the hub has answered - this is what the app
		// does: it sends its hello the moment the socket opens.
		payload := `{"type":"hello","v":1,"token":"a-token"}`
		if err := client.Write(ctx, websocket.MessageText, []byte(payload)); err != nil {
			t.Fatal(err)
		}
		_, data, err := tunnel.Read(ctx)
		if err != nil {
			t.Fatal(err)
		}
		open := string(data)
		// Other clients come and go on the same tunnel; wait for our own !open:.
		for !strings.HasPrefix(open, "!open:") {
			if !strings.HasPrefix(open, "!close:") {
				t.Fatalf("want !open:<cid>, got %q", open)
			}
			if _, data, err = tunnel.Read(ctx); err != nil {
				t.Fatal(err)
			}
			open = string(data)
		}
		cid := parseCID(strings.TrimPrefix(open, "!open:"))
		// Wait until the frame really waits in the relay's buffer: otherwise the
		// test would depend on a race between the client and the tunnel.
		h := r.getHub("home")
		buffered := false
		deadline := time.Now().Add(2 * time.Second)
		for !buffered && time.Now().Before(deadline) {
			h.mu.Lock()
			b := h.buffered[cid]
			buffered = b != nil && len(b.frames) > 0
			h.mu.Unlock()
			if !buffered {
				time.Sleep(5 * time.Millisecond)
			}
		}
		if !buffered {
			t.Fatal("the frame must wait for the hub")
		}
		if err := tunnel.Write(ctx, websocket.MessageText, []byte(fmt.Sprintf("!ready:%d", cid))); err != nil {
			t.Fatal(err)
		}
		_, data, err = tunnel.Read(ctx)
		if err != nil {
			t.Fatal(err)
		}
		// The tunnel speaks "<cid>:<payload>"; a frame without the id is dropped
		// by the firmware and the client's hello never arrives.
		want := fmt.Sprintf("%d:%s", cid, payload)
		if string(data) != want {
			t.Fatalf("want %q, got %q", want, string(data))
		}
	})

	t.Run("a hub with a secret of its own uses it", func(t *testing.T) {
		soloCtx, soloCancel := context.WithCancel(ctx)
		defer soloCancel()
		solo, _, err := websocket.Dial(soloCtx, wsBase+"/tunnel/solo", nil)
		if err != nil {
			t.Fatalf("the tunnel must connect: %v", err)
		}
		if err := solo.Write(ctx, websocket.MessageText, []byte("!auth:the-secrets-of-one-hub-only")); err != nil {
			t.Fatal(err)
		}
		_, data, err := solo.Read(ctx)
		if err != nil {
			t.Fatal(err)
		}
		if string(data) != "!ok" {
			t.Fatalf("want !ok, got %q", data)
		}
		solo.Close(websocket.StatusNormalClosure, "")

		// The relay's shared secret must not open a hub that has a secret of its own.
		again, _, err := websocket.Dial(soloCtx, wsBase+"/tunnel/solo", nil)
		if err != nil {
			t.Fatalf("the tunnel must connect: %v", err)
		}
		defer again.Close(websocket.StatusNormalClosure, "")
		if err := again.Write(ctx, websocket.MessageText, []byte("!auth:"+secret)); err != nil {
			t.Fatal(err)
		}
		_, data, err = again.Read(ctx)
		if err != nil {
			t.Fatal(err)
		}
		if !strings.HasPrefix(string(data), "!error:unauthorized") {
			t.Fatalf("the shared secret must not open this hub, got %q", data)
		}
	})

	t.Run("a hub whose secret is derived from the master registers", func(t *testing.T) {
		// Nothing was added to the relay for this hub: it derives the secret
		// itself, so a new home needs no configuration there.
		masterRelay := newRelay("", "", nil, MaxClientsPerHub, MaxHubs)
		masterRelay.setMaster("the-master-key")
		masterSrv := httptest.NewServer(masterRelay)
		defer masterSrv.Close()
		base := "ws" + strings.TrimPrefix(masterSrv.URL, "http")

		tunnelCtx, tunnelCancel := context.WithCancel(ctx)
		defer tunnelCancel()
		tunnel, _, err := websocket.Dial(tunnelCtx, base+"/tunnel/kitchen", nil)
		if err != nil {
			t.Fatalf("the tunnel must connect: %v", err)
		}
		defer tunnel.Close(websocket.StatusNormalClosure, "")
		if err := tunnel.Write(ctx, websocket.MessageText, []byte("!auth:"+DeriveHubSecret("the-master-key", "kitchen"))); err != nil {
			t.Fatal(err)
		}
		_, data, err := tunnel.Read(ctx)
		if err != nil {
			t.Fatal(err)
		}
		if string(data) != "!ok" {
			t.Fatalf("want !ok, got %q", data)
		}
	})

	t.Run("a wrong secret is refused", func(t *testing.T) {
		bad, _, err := websocket.Dial(ctx, wsBase+"/tunnel/home", nil)
		if err != nil {
			t.Fatal(err)
		}
		defer bad.Close(websocket.StatusNormalClosure, "")
		if err := bad.Write(ctx, websocket.MessageText, []byte("!auth:wrong-secret")); err != nil {
			t.Fatal(err)
		}
		_, data, err := bad.Read(ctx)
		if err != nil {
			t.Fatal(err)
		}
		if !strings.HasPrefix(string(data), "!error:unauthorized") {
			t.Fatalf("want !error:unauthorized, got %q", data)
		}
	})

	t.Run("the source is blocked after five failures", func(t *testing.T) {
		for i := 0; i < 5; i++ {
			w, _, err := websocket.Dial(ctx, wsBase+"/tunnel/home", nil)
			if err != nil {
				t.Fatal(err)
			}
			_ = w.Write(ctx, websocket.MessageText, []byte("!auth:wrong-secret"))
			_, _, _ = w.Read(ctx)
			w.Close(websocket.StatusNormalClosure, "")
		}
		blocked, _, err := websocket.Dial(ctx, wsBase+"/tunnel/home", nil)
		if err != nil {
			t.Fatal(err)
		}
		defer blocked.Close(websocket.StatusNormalClosure, "")
		if err := blocked.Write(ctx, websocket.MessageText, []byte("!auth:wrong-secret")); err != nil {
			t.Fatal(err)
		}
		_, data, err := blocked.Read(ctx)
		if err != nil {
			t.Fatal(err)
		}
		if !strings.Contains(string(data), "rate limited") {
			t.Fatalf("want a rate limit, got %q", data)
		}
	})
}
