package main

import (
	"io"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"github.com/kliuchnikovv/keystone-esp/relay/internal/ws"
	"github.com/kliuchnikovv/keystone-esp/relay/internal/wstest"
)

const testSecret = "0123456789abcdef0123456789abcdef"

func newTestServer(t *testing.T, maxClients int) (string, *httptest.Server) {
	t.Helper()
	s := &server{
		secret:       testSecret,
		hubs:         newRegistry(),
		maxClients:   maxClients,
		pingInterval: 0, // no keepalive in tests: it would only add frames to read
		authTimeout:  2 * time.Second,
		wsOpts:       ws.Options{MaxMessage: 1 << 20, ReadTimeout: 5 * time.Second},
	}
	mux := http.NewServeMux()
	mux.HandleFunc("/tunnel/", func(w http.ResponseWriter, r *http.Request) {
		s.handleTunnel(w, r, pathID(r.URL.Path, "tunnel"))
	})
	mux.HandleFunc("/client/", func(w http.ResponseWriter, r *http.Request) {
		s.handleClient(w, r, pathID(r.URL.Path, "client"))
	})
	mux.HandleFunc("/", s.statusPage)
	srv := httptest.NewServer(mux)
	t.Cleanup(srv.Close)
	return strings.TrimPrefix(srv.URL, "http://"), srv
}

func dial(t *testing.T, base, kind, hubID string) *wstest.Client {
	t.Helper()
	c, err := wstest.Dial("ws://" + base + "/" + kind + "/" + hubID)
	if err != nil {
		t.Fatalf("dial %s/%s: %v", kind, hubID, err)
	}
	t.Cleanup(func() { _ = c.Close() })
	c.SetReadDeadline(time.Now().Add(3 * time.Second))
	return c
}

// attachTunnel dials a tunnel and completes the handshake.
func attachTunnel(t *testing.T, base, hubID string) *wstest.Client {
	t.Helper()
	c := dial(t, base, "tunnel", hubID)
	if err := c.WriteText("!auth:" + testSecret); err != nil {
		t.Fatalf("auth: %v", err)
	}
	op, msg, err := c.ReadMessage()
	if err != nil {
		t.Fatalf("read !ok: %v", err)
	}
	if op != ws.OpText || string(msg) != "!ok" {
		t.Fatalf("got op=%d %q, want !ok", op, msg)
	}
	return c
}

// expectClosed waits for the relay to hang up: a close frame, or the socket simply gone.
func expectClosed(t *testing.T, c *wstest.Client) {
	t.Helper()
	c.SetReadDeadline(time.Now().Add(3 * time.Second))
	op, msg, err := c.ReadMessage()
	if err != nil {
		return
	}
	if op == ws.OpClose {
		return
	}
	t.Fatalf("expected a close, got opcode %d (%q)", op, truncate(msg, 40))
}

func readText(t *testing.T, c *wstest.Client) string {
	t.Helper()
	c.SetReadDeadline(time.Now().Add(3 * time.Second))
	op, msg, err := c.ReadMessage()
	if err != nil {
		t.Fatalf("read: %v", err)
	}
	if op != ws.OpText {
		t.Fatalf("opcode = %d, want text", op)
	}
	return string(msg)
}

func TestTunnelAuthenticatesWithTheSecret(t *testing.T) {
	base, _ := newTestServer(t, 16)
	c := dial(t, base, "tunnel", "home")
	if err := c.WriteText("!auth:" + testSecret); err != nil {
		t.Fatalf("auth: %v", err)
	}
	if got := readText(t, c); got != "!ok" {
		t.Fatalf("got %q, want !ok", got)
	}
}

func TestTunnelWithABadSecretIsRefused(t *testing.T) {
	base, _ := newTestServer(t, 16)
	c := dial(t, base, "tunnel", "home")
	if err := c.WriteText("!auth:wrong-secret-entirely"); err != nil {
		t.Fatalf("auth: %v", err)
	}
	c.SetReadDeadline(time.Now().Add(3 * time.Second))
	_, msg, err := c.ReadMessage()
	if err != nil {
		t.Fatalf("read: %v", err)
	}
	if string(msg) != "!error:unauthorized" {
		t.Fatalf("got %q, want !error:unauthorized", msg)
	}
	// The relay hangs up right after the refusal.
	expectClosed(t, c)
}

func TestTunnelThatSendsNothingIsDropped(t *testing.T) {
	s := &server{secret: testSecret, hubs: newRegistry(), authTimeout: 150 * time.Millisecond,
		wsOpts: ws.Options{ReadTimeout: time.Second}}
	mux := http.NewServeMux()
	mux.HandleFunc("/tunnel/", func(w http.ResponseWriter, r *http.Request) {
		s.handleTunnel(w, r, pathID(r.URL.Path, "tunnel"))
	})
	srv := httptest.NewServer(mux)
	defer srv.Close()

	c, err := wstest.Dial("ws://" + strings.TrimPrefix(srv.URL, "http://") + "/tunnel/silent")
	if err != nil {
		t.Fatalf("dial: %v", err)
	}
	defer c.Close()
	expectClosed(t, c)
}

func TestClientBeforeAnyHubIsRefused(t *testing.T) {
	base, _ := newTestServer(t, 16)
	resp, err := http.Get("http://" + base + "/")
	if err != nil {
		t.Fatalf("status page: %v", err)
	}
	body, _ := io.ReadAll(resp.Body)
	_ = resp.Body.Close()
	if !strings.Contains(string(body), "Keystone Relay") {
		t.Fatalf("unexpected status page: %q", body)
	}

	// No WebSocket upgrade: the relay answers with a plain 503.
	plain, err := http.Get("http://" + base + "/client/nobody")
	if err != nil {
		t.Fatalf("client before hub: %v", err)
	}
	defer plain.Body.Close()
	if plain.StatusCode != http.StatusServiceUnavailable {
		t.Fatalf("got %d, want 503 when the hub is not connected", plain.StatusCode)
	}
}

// The whole point of the protocol: a client is opened by the hub, held until the hub says
// !ready, and then both directions carry payloads the relay never looks at.
func TestClientIsOpenedHeldAndForwardedBothWays(t *testing.T) {
	base, _ := newTestServer(t, 16)
	tun := attachTunnel(t, base, "home")

	cli := dial(t, base, "client", "home")
	if got := readText(t, tun); got != "!open:1" {
		t.Fatalf("hub got %q, want !open:1", got)
	}

	// Client → hub before !ready: the relay must hold it.
	if err := cli.WriteText(`{"type":"hello"}`); err != nil {
		t.Fatalf("client write: %v", err)
	}
	tun.SetReadDeadline(time.Now().Add(300 * time.Millisecond))
	if _, _, err := tun.ReadMessage(); err == nil {
		t.Fatal("the relay forwarded a client frame before the hub was ready")
	}

	if err := tun.WriteText("!ready:1"); err != nil {
		t.Fatalf("ready: %v", err)
	}
	if got := readText(t, tun); got != `1:{"type":"hello"}` {
		t.Fatalf("hub got %q, want the held frame with its cid", got)
	}

	// Hub → client passes straight through.
	if err := tun.WriteText(`1:{"type":"welcome"}`); err != nil {
		t.Fatalf("hub write: %v", err)
	}
	if got := readText(t, cli); got != `{"type":"welcome"}` {
		t.Fatalf("client got %q, want the payload without the cid prefix", got)
	}

	// A frame for a client that is gone must not confuse anyone.
	if err := tun.WriteText("7:{\"type\":\"nope\"}"); err != nil {
		t.Fatalf("hub write: %v", err)
	}

	// The hub closes the client: it must learn about it.
	if err := tun.WriteText("!close:1"); err != nil {
		t.Fatalf("close: %v", err)
	}
	expectClosed(t, cli)
}

// The hub refuses a client (bad token, bad JSON, busy) with an error and then closes it.
// No !ready is ever sent, so that error must go out at once instead of waiting for one.
func TestHubReachesAClientThatNeverGotReady(t *testing.T) {
	base, _ := newTestServer(t, 16)
	tun := attachTunnel(t, base, "home")

	cli := dial(t, base, "client", "home")
	if got := readText(t, tun); got != "!open:1" {
		t.Fatalf("hub got %q", got)
	}
	if err := tun.WriteText(`1:{"type":"error","code":"unauthorized"}`); err != nil {
		t.Fatalf("hub write: %v", err)
	}
	if got := readText(t, cli); got != `{"type":"error","code":"unauthorized"}` {
		t.Fatalf("client got %q, want the refusal without waiting for !ready", got)
	}
}

// The relay is transparent: a binary frame is not its business to judge, it is forwarded so
// the hub can refuse it (a client that sends binary must get an answer, not silence).
func TestBinaryFrameIsForwardedNotSwallowed(t *testing.T) {
	base, _ := newTestServer(t, 16)
	tun := attachTunnel(t, base, "home")
	cli := dial(t, base, "client", "home")
	if got := readText(t, tun); got != "!open:1" {
		t.Fatalf("hub got %q", got)
	}
	if err := tun.WriteText("!ready:1"); err != nil {
		t.Fatalf("ready: %v", err)
	}

	if err := cli.WriteFrameRaw(ws.OpBinary, true, []byte{1, 2, 3}); err != nil {
		t.Fatalf("binary write: %v", err)
	}
	if got := readText(t, tun); got != "1:\x01\x02\x03" {
		t.Fatalf("hub got %q, want the bytes with the cid in front", got)
	}

	// And the other way: a binary frame from the hub stays binary.
	if err := tun.WriteFrameRaw(ws.OpBinary, true, []byte(`1:{"type":"error","code":"bad_json"}`)); err != nil {
		t.Fatalf("binary from hub: %v", err)
	}
	cli.SetReadDeadline(time.Now().Add(3 * time.Second))
	op, msg, err := cli.ReadMessage()
	if err != nil {
		t.Fatalf("read: %v", err)
	}
	if op != ws.OpBinary || string(msg) != `{"type":"error","code":"bad_json"}` {
		t.Fatalf("client got op=%d %q, want the same binary frame back", op, msg)
	}
}

func TestClientDisconnectIsToldToTheHub(t *testing.T) {
	base, _ := newTestServer(t, 16)
	tun := attachTunnel(t, base, "home")

	cli := dial(t, base, "client", "home")
	if got := readText(t, tun); got != "!open:1" {
		t.Fatalf("hub got %q", got)
	}
	if err := tun.WriteText("!ready:1"); err != nil {
		t.Fatalf("ready: %v", err)
	}
	_ = cli.Close()

	// The relay must report the closure, otherwise the hub keeps a dead virtual client.
	deadline := time.Now().Add(3 * time.Second)
	for time.Now().Before(deadline) {
		if got := readText(t, tun); got == "!close:1" {
			return
		}
	}
	t.Fatal("the hub was never told the client disconnected")
}

func TestBackstopRefusesTheClientPastTheLimit(t *testing.T) {
	base, _ := newTestServer(t, 1)
	tun := attachTunnel(t, base, "home")

	_ = dial(t, base, "client", "home")
	if got := readText(t, tun); got != "!open:1" {
		t.Fatalf("hub got %q", got)
	}

	extra := dial(t, base, "client", "home")
	extra.SetReadDeadline(time.Now().Add(3 * time.Second))
	op, msg, err := extra.ReadMessage()
	if err != nil {
		t.Fatalf("the refused client got no answer at all: %v", err)
	}
	code, _ := wstest.CloseCode(msg)
	if op != ws.OpClose || code != ws.CloseTooManyUsers {
		t.Fatalf("got op=%d code=%d, want a close with 4429", op, code)
	}
}

func TestAReconnectingHubReplacesTheOldTunnelAndDropsItsClients(t *testing.T) {
	base, _ := newTestServer(t, 16)
	tun1 := attachTunnel(t, base, "home")

	cli := dial(t, base, "client", "home")
	if got := readText(t, tun1); got != "!open:1" {
		t.Fatalf("first hub got %q", got)
	}

	tun2 := attachTunnel(t, base, "home") // the hub reconnected

	// The client of the old tunnel must be gone, not left hanging on a dead connection.
	expectClosed(t, cli)

	// And the new tunnel serves new clients.
	dial(t, base, "client", "home")
	if got := readText(t, tun2); got != "!open:1" {
		t.Fatalf("second hub got %q, want a fresh cid", got)
	}
}

func TestFramesForUnknownControlWordsAreIgnored(t *testing.T) {
	base, _ := newTestServer(t, 16)
	tun := attachTunnel(t, base, "home")

	if err := tun.WriteText("!nonsense:1"); err != nil {
		t.Fatalf("write: %v", err)
	}
	if err := tun.WriteText("garbage-without-a-cid"); err != nil {
		t.Fatalf("write: %v", err)
	}

	// The tunnel must still work afterwards.
	cli := dial(t, base, "client", "home")
	if got := readText(t, tun); got != "!open:1" {
		t.Fatalf("hub got %q, want !open:1 after the junk", got)
	}
	_ = cli
}

func TestPathIDRejectsRubbish(t *testing.T) {
	cases := map[string]string{
		"/tunnel/home":  "home",
		"/tunnel/a/b":   "",
		"/tunnel/":      "",
		"/tunnel":       "",
		"/tunnel//home": "",
	}
	if got := pathID("/client/home", "client"); got != "home" {
		t.Errorf("pathID(/client/home, client) = %q, want home", got)
	}
	if got := pathID("/tunnel/home", "client"); got != "" {
		t.Errorf("a tunnel path handed to the client handler must not parse, got %q", got)
	}
	for path, want := range cases {
		kind := "tunnel"
		if strings.HasPrefix(path, "/client/") {
			kind = "client"
		}
		if got := pathID(path, kind); got != want {
			t.Errorf("pathID(%q, %q) = %q, want %q", path, kind, got, want)
		}
	}
}

func TestReadTimeoutForGivesASlowPeerRoom(t *testing.T) {
	if got := readTimeoutFor(0, 30*time.Second); got != 70*time.Second {
		t.Errorf("readTimeoutFor(0, 30s) = %v, want 70s (one missed ping is not a dead peer)", got)
	}
	if got := readTimeoutFor(45*time.Second, 30*time.Second); got != 45*time.Second {
		t.Errorf("an explicit --read-timeout must win, got %v", got)
	}
	if got := readTimeoutFor(0, 0); got != 0 {
		t.Errorf("with keepalives off there is no deadline, got %v", got)
	}
}

func TestParseCid(t *testing.T) {
	if cid, payload, ok := parseCid([]byte(`42:{"type":"cmd"}`)); !ok || cid != 42 || string(payload) != `{"type":"cmd"}` {
		t.Fatalf("got cid=%d payload=%q ok=%v", cid, payload, ok)
	}
	for _, bad := range []string{"", ":x", "abc:x", "0:x", "-1:x", "nocolon"} {
		if _, _, ok := parseCid([]byte(bad)); ok {
			t.Errorf("parseCid(%q) was accepted", bad)
		}
	}
}
