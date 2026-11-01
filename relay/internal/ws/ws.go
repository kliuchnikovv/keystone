// Package ws is a small RFC 6455 WebSocket server: handshake, text/binary frames,
// fragmentation, ping/pong and close. No extensions, no compression, no dependencies —
// the Keystone relay is deployed as a single static binary on a VPS and should not drag a
// module graph along with it.
//
// It is server-side only: every frame from a client is masked, every frame we send is not.
package ws

import (
	"bufio"
	"crypto/sha1"
	"encoding/base64"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"net"
	"net/http"
	"strings"
	"sync"
	"sync/atomic"
	"time"
)

// Frame opcodes (RFC 6455 §5.2).
const (
	OpContinuation byte = 0x0
	OpText         byte = 0x1
	OpBinary       byte = 0x2
	OpClose        byte = 0x8
	OpPing         byte = 0x9
	OpPong         byte = 0xA
)

// Close codes we use (RFC 6455 §7.4 and the WebSocket application registry).
const (
	CloseNormal       uint16 = 1000
	CloseGoingAway    uint16 = 1001
	CloseProtocol     uint16 = 1002
	CloseTooBig       uint16 = 1009
	ClosePolicy       uint16 = 1008
	CloseHubNotThere  uint16 = 4404
	CloseTooManyUsers uint16 = 4429
)

const handshakeGUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

// Errors a caller may want to tell apart.
var (
	ErrProtocol = errors.New("ws: protocol error")
	ErrTooLarge = errors.New("ws: message too large")
	ErrClosed   = errors.New("ws: connection closed")
)

// Options for Upgrade.
type Options struct {
	// MaxMessage caps one assembled message (default 1 MiB). The hub's frames are at most
	// 4 KB, so anything far above that is a broken or hostile peer.
	MaxMessage int
	// ReadTimeout bounds how long one ReadMessage may block (0 = no bound). Set it to a
	// little more than the keepalive interval: every pong resets it, so a peer that stops
	// answering — a dead NAT mapping, a frozen client — is noticed instead of lingering.
	ReadTimeout time.Duration
}

// Conn is one WebSocket connection. Reads are done by a single goroutine, writes may come
// from several and are serialised, so a frame never interleaves with another.
type Conn struct {
	conn        net.Conn
	br          *bufio.Reader
	maxMsg      int
	readTimeout time.Duration
	wmu         sync.Mutex
	closed      atomic.Bool
	closeMu     sync.Mutex
}

// Upgrade performs the handshake and takes the connection over from net/http.
func Upgrade(w http.ResponseWriter, r *http.Request, opt Options) (*Conn, error) {
	if !headerHasToken(r.Header, "Connection", "upgrade") {
		http.Error(w, "expected an Upgrade connection", http.StatusBadRequest)
		return nil, errors.New("ws: no upgrade header")
	}
	if !strings.EqualFold(strings.TrimSpace(r.Header.Get("Upgrade")), "websocket") {
		http.Error(w, "expected a websocket upgrade", http.StatusBadRequest)
		return nil, errors.New("ws: not a websocket upgrade")
	}
	if v := strings.TrimSpace(r.Header.Get("Sec-WebSocket-Version")); v != "13" {
		w.Header().Set("Sec-WebSocket-Version", "13")
		http.Error(w, "only WebSocket version 13", http.StatusUpgradeRequired)
		return nil, errors.New("ws: unsupported version " + v)
	}
	key := r.Header.Get("Sec-WebSocket-Key")
	if key == "" {
		http.Error(w, "missing Sec-WebSocket-Key", http.StatusBadRequest)
		return nil, errors.New("ws: missing key")
	}

	hj, ok := w.(http.Hijacker)
	if !ok {
		http.Error(w, "connection cannot be hijacked", http.StatusInternalServerError)
		return nil, errors.New("ws: response writer does not support hijacking")
	}
	conn, brw, err := hj.Hijack()
	if err != nil {
		return nil, err
	}

	sum := sha1.Sum([]byte(key + handshakeGUID)) // #nosec G401 — required by the RFC
	accept := base64.StdEncoding.EncodeToString(sum[:])
	resp := "HTTP/1.1 101 Switching Protocols\r\n" +
		"Upgrade: websocket\r\n" +
		"Connection: Upgrade\r\n" +
		"Sec-WebSocket-Accept: " + accept + "\r\n\r\n"
	if _, err := brw.Writer.WriteString(resp); err != nil {
		conn.Close()
		return nil, err
	}
	if err := brw.Writer.Flush(); err != nil {
		conn.Close()
		return nil, err
	}

	max := opt.MaxMessage
	if max <= 0 {
		max = 1 << 20
	}
	return &Conn{conn: conn, br: brw.Reader, maxMsg: max, readTimeout: opt.ReadTimeout}, nil
}

func headerHasToken(h http.Header, key, token string) bool {
	for _, v := range h.Values(key) {
		for _, part := range strings.Split(v, ",") {
			if strings.EqualFold(strings.TrimSpace(part), token) {
				return true
			}
		}
	}
	return false
}

func (c *Conn) readFrame() (fin bool, opcode byte, payload []byte, err error) {
	if c.readTimeout > 0 {
		_ = c.conn.SetReadDeadline(time.Now().Add(c.readTimeout))
	}
	var hdr [2]byte
	if _, err = io.ReadFull(c.br, hdr[:]); err != nil {
		return false, 0, nil, err
	}
	fin = hdr[0]&0x80 != 0
	if hdr[0]&0x70 != 0 { // reserved bits without a negotiated extension
		return false, 0, nil, ErrProtocol
	}
	opcode = hdr[0] & 0x0F
	masked := hdr[1]&0x80 != 0
	if !masked { // RFC 6455 §5.1: a client must mask
		return false, 0, nil, ErrProtocol
	}

	length := int64(hdr[1] & 0x7F)
	switch length {
	case 126:
		var b [2]byte
		if _, err = io.ReadFull(c.br, b[:]); err != nil {
			return false, 0, nil, err
		}
		length = int64(binary.BigEndian.Uint16(b[:]))
	case 127:
		var b [8]byte
		if _, err = io.ReadFull(c.br, b[:]); err != nil {
			return false, 0, nil, err
		}
		u := binary.BigEndian.Uint64(b[:])
		if u > 1<<62 {
			return false, 0, nil, ErrTooLarge
		}
		length = int64(u)
	}
	if length > int64(c.maxMsg) {
		return false, 0, nil, ErrTooLarge
	}

	var mask [4]byte
	if _, err = io.ReadFull(c.br, mask[:]); err != nil {
		return false, 0, nil, err
	}
	payload = make([]byte, length)
	if _, err = io.ReadFull(c.br, payload); err != nil {
		return false, 0, nil, err
	}
	for i := range payload {
		payload[i] ^= mask[i%4]
	}

	if opcode >= 0x8 && (!fin || length > 125) { // control frames (RFC 6455 §5.5)
		return false, 0, nil, ErrProtocol
	}
	return fin, opcode, payload, nil
}

// ReadMessage returns one complete message with its fragments assembled. Control frames
// (ping, pong, close) come back as themselves, immediately, and do not touch reassembly
// state. Only one goroutine may read.
func (c *Conn) ReadMessage() (opcode byte, payload []byte, err error) {
	var msg []byte
	var op byte
	for {
		var fin bool
		fin, code, data, err := c.readFrame()
		if err != nil {
			return 0, nil, err
		}
		switch code {
		case OpPing, OpPong, OpClose:
			return code, data, nil
		case OpContinuation:
			if op == 0 {
				return 0, nil, ErrProtocol
			}
			msg = append(msg, data...)
		case OpText, OpBinary:
			if op != 0 { // a new message before the previous one finished
				return 0, nil, ErrProtocol
			}
			op, msg = code, append(msg, data...)
		default:
			return 0, nil, ErrProtocol
		}
		if fin && op != 0 {
			return op, msg, nil
		}
		if len(msg) > c.maxMsg {
			return 0, nil, ErrTooLarge
		}
	}
}

func (c *Conn) writeFrame(opcode byte, payload []byte) error {
	if c.closed.Load() {
		return ErrClosed
	}
	c.wmu.Lock()
	defer c.wmu.Unlock()

	hdr := make([]byte, 0, 10)
	hdr = append(hdr, 0x80|opcode) // FIN, server frames are never masked
	switch n := len(payload); {
	case n < 126:
		hdr = append(hdr, byte(n))
	case n <= 0xFFFF:
		hdr = append(hdr, 126, byte(n>>8), byte(n))
	default:
		var b [8]byte
		binary.BigEndian.PutUint64(b[:], uint64(n))
		hdr = append(hdr, 127)
		hdr = append(hdr, b[:]...)
	}

	// One buffer, one write: a frame can never end up half-written under a concurrent sender.
	buf := make([]byte, 0, len(hdr)+len(payload))
	buf = append(buf, hdr...)
	buf = append(buf, payload...)
	_, err := c.conn.Write(buf)
	return err
}

// WriteText sends one text message.
func (c *Conn) WriteText(b []byte) error { return c.writeFrame(OpText, b) }

// WriteBinary sends one binary message.
func (c *Conn) WriteBinary(b []byte) error { return c.writeFrame(OpBinary, b) }

// WriteMessage sends a data message with the given opcode (OpText or OpBinary), so a frame
// that arrived as binary leaves as binary and the relay stays transparent.
func (c *Conn) WriteMessage(opcode byte, b []byte) error { return c.writeFrame(opcode, b) }

// WritePing sends a ping (payload must be at most 125 bytes).
func (c *Conn) WritePing(b []byte) error {
	if len(b) > 125 {
		b = b[:125]
	}
	return c.writeFrame(OpPing, b)
}

// WritePong answers a ping.
func (c *Conn) WritePong(b []byte) error { return c.writeFrame(OpPong, b) }

// WriteClose sends a close frame with a code and a short reason, then closes the socket.
func (c *Conn) WriteClose(code uint16, reason string) error {
	if len(reason) > 120 {
		reason = reason[:120]
	}
	payload := make([]byte, 2+len(reason))
	binary.BigEndian.PutUint16(payload[:2], code)
	copy(payload[2:], reason)
	err := c.writeFrame(OpClose, payload)
	c.close()
	return err
}

// Close tears the socket down without a close handshake.
func (c *Conn) Close() error { return c.close() }

func (c *Conn) close() error {
	if c.closed.Swap(true) {
		return nil
	}
	return c.conn.Close()
}

// Closed reports whether the connection is gone.
func (c *Conn) Closed() bool { return c.closed.Load() }

// SetReadDeadline bounds how long a read may block; the caller uses it to notice a peer
// that stopped answering pings.
func (c *Conn) SetReadDeadline(t time.Time) error { return c.conn.SetReadDeadline(t) }

// RemoteAddr is the peer's address, for logging.
func (c *Conn) RemoteAddr() string { return c.conn.RemoteAddr().String() }

// Keepalive pings the peer every interval until the connection is closed. It only writes:
// the read deadline belongs to the reader (see Options.ReadTimeout), because a peer that
// never answers is a missing read, not a failed write.
func (c *Conn) Keepalive(interval time.Duration, onError func(error)) {
	if interval <= 0 {
		return
	}
	go func() {
		ticker := time.NewTicker(interval)
		defer ticker.Stop()
		for range ticker.C {
			if c.Closed() {
				return
			}
			if err := c.WritePing(nil); err != nil {
				if onError != nil && !c.Closed() {
					onError(fmt.Errorf("ws: ping to %s: %w", c.RemoteAddr(), err))
				}
				c.Close()
				return
			}
		}
	}()
}
