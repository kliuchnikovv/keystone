package ws_test

import (
	"errors"
	"net/http"
	"net/http/httptest"
	"os"
	"strings"
	"testing"
	"time"

	"github.com/kliuchnikovv/keystone-esp/relay/internal/ws"
	"github.com/kliuchnikovv/keystone-esp/relay/internal/wstest"
)

// echoServer upgrades and echoes every text message back, answering pings itself.
func echoServer(t *testing.T, maxMsg int) *httptest.Server {
	t.Helper()
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		c, err := ws.Upgrade(w, r, ws.Options{MaxMessage: maxMsg})
		if err != nil {
			return
		}
		defer c.Close()
		for {
			op, msg, err := c.ReadMessage()
			if err != nil {
				return
			}
			switch op {
			case ws.OpPing:
				_ = c.WritePong(msg)
			case ws.OpClose:
				_ = c.WriteClose(ws.CloseNormal, "bye")
				return
			case ws.OpPong:
			case ws.OpText, ws.OpBinary:
				_ = c.WriteText(msg)
			}
		}
	}))
	t.Cleanup(srv.Close)
	return srv
}

func mustDial(t *testing.T, url string) *wstest.Client {
	t.Helper()
	c, err := wstest.Dial(url)
	if err != nil {
		t.Fatalf("dial %s: %v", url, err)
	}
	t.Cleanup(func() { _ = c.Close() })
	return c
}

func readWithTimeout(t *testing.T, c *wstest.Client) (byte, []byte) {
	t.Helper()
	c.SetReadDeadline(time.Now().Add(3 * time.Second))
	op, msg, err := c.ReadMessage()
	if err != nil {
		t.Fatalf("read: %v", err)
	}
	return op, msg
}

func TestTextRoundTrip(t *testing.T) {
	srv := echoServer(t, 1<<20)
	c := mustDial(t, "ws://"+strings.TrimPrefix(srv.URL, "http://"))

	for _, payload := range []string{"", "hello", "!ok", strings.Repeat("x", 200), strings.Repeat("y", 70000)} {
		if err := c.WriteText(payload); err != nil {
			t.Fatalf("write %d B: %v", len(payload), err)
		}
		op, got := readWithTimeout(t, c)
		if op != ws.OpText {
			t.Fatalf("opcode = %d, want text", op)
		}
		if string(got) != payload {
			t.Fatalf("echo of %d B differs (got %d B)", len(payload), len(got))
		}
	}
}

func TestFragmentedMessageIsAssembled(t *testing.T) {
	srv := echoServer(t, 1<<20)
	c := mustDial(t, "ws://"+strings.TrimPrefix(srv.URL, "http://"))

	if err := c.WriteFrameRaw(ws.OpText, false, []byte("part1-")); err != nil {
		t.Fatalf("first fragment: %v", err)
	}
	if err := c.WriteFrameRaw(ws.OpContinuation, false, []byte("part2-")); err != nil {
		t.Fatalf("second fragment: %v", err)
	}
	if err := c.WriteFrameRaw(ws.OpContinuation, true, []byte("part3")); err != nil {
		t.Fatalf("last fragment: %v", err)
	}

	op, got := readWithTimeout(t, c)
	if op != ws.OpText || string(got) != "part1-part2-part3" {
		t.Fatalf("got op=%d %q", op, got)
	}
}

func TestPingIsAnsweredAndPongPassesThrough(t *testing.T) {
	srv := echoServer(t, 1<<20)
	c := mustDial(t, "ws://"+strings.TrimPrefix(srv.URL, "http://"))

	if err := c.WriteFrameRaw(ws.OpPing, true, []byte("ping-payload")); err != nil {
		t.Fatalf("ping: %v", err)
	}
	op, got := readWithTimeout(t, c)
	if op != ws.OpPong || string(got) != "ping-payload" {
		t.Fatalf("got op=%d %q, want pong with the same payload", op, got)
	}
}

func TestCloseIsAnsweredWithACode(t *testing.T) {
	srv := echoServer(t, 1<<20)
	c := mustDial(t, "ws://"+strings.TrimPrefix(srv.URL, "http://"))

	if err := c.WriteFrameRaw(ws.OpClose, true, []byte{0x03, 0xE8, 'b', 'y', 'e'}); err != nil {
		t.Fatalf("close: %v", err)
	}
	op, got := readWithTimeout(t, c)
	code, _ := wstest.CloseCode(got)
	if op != ws.OpClose || code != 1000 {
		t.Fatalf("got op=%d code=%d, want a close with 1000", op, code)
	}
}

func TestMessageOverTheLimitClosesTheConnection(t *testing.T) {
	srv := echoServer(t, 1024) // the hub's frames are at most 4 KB; the cap is ours to set
	c := mustDial(t, "ws://"+strings.TrimPrefix(srv.URL, "http://"))

	if err := c.WriteText(strings.Repeat("z", 4096)); err != nil {
		t.Fatalf("write: %v", err)
	}
	c.SetReadDeadline(time.Now().Add(3 * time.Second))
	if _, _, err := c.ReadMessage(); err == nil {
		t.Fatal("the connection stayed open after an oversized frame")
	}
}

func TestHandshakeRejectsAPlainRequest(t *testing.T) {
	srv := echoServer(t, 1<<20)
	resp, err := http.Get(srv.URL)
	if err != nil {
		t.Fatalf("get: %v", err)
	}
	defer resp.Body.Close()
	if resp.StatusCode == http.StatusSwitchingProtocols {
		t.Fatal("a plain GET was upgraded")
	}
}

func TestUnmaskedClientFrameIsAProtocolError(t *testing.T) {
	srv := echoServer(t, 1<<20)
	c := mustDial(t, "ws://"+strings.TrimPrefix(srv.URL, "http://"))

	// A client frame without the mask bit must be rejected (RFC 6455 §5.1).
	if err := c.WriteFrameUnmasked(ws.OpText, []byte("nope")); err != nil {
		t.Fatalf("write: %v", err)
	}
	c.SetReadDeadline(time.Now().Add(3 * time.Second))
	if _, _, err := c.ReadMessage(); err == nil {
		t.Fatal("an unmasked frame was accepted")
	}
}

// A peer that stops answering pings must surface as a read timeout, not linger forever.
func TestSilentPeerTripsTheReadTimeout(t *testing.T) {
	done := make(chan error, 1)
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		c, err := ws.Upgrade(w, r, ws.Options{ReadTimeout: 200 * time.Millisecond})
		if err != nil {
			return
		}
		defer c.Close()
		c.Keepalive(20*time.Millisecond, nil)
		// The client never answers, so every pong is missing and the deadline fires.
		_, _, err = c.ReadMessage()
		done <- err
	}))
	defer srv.Close()

	mustDial(t, "ws://"+strings.TrimPrefix(srv.URL, "http://"))
	select {
	case err := <-done:
		if err == nil {
			t.Fatal("the read returned no error for a silent peer")
		}
		if !errors.Is(err, os.ErrDeadlineExceeded) {
			t.Fatalf("got %v, want a deadline error", err)
		}
	case <-time.After(3 * time.Second):
		t.Fatal("the silent peer was never noticed")
	}
}
