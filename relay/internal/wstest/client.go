// Package wstest is a tiny client-side WebSocket, used by the relay's tests to drive the
// server exactly like a hub or a phone would: masked frames, the real handshake, real
// control frames. Hand-rolled on purpose — a test that reuses the server's own code would
// agree with it about everything, including mistakes.
package wstest

import (
	"bufio"
	"crypto/rand"
	"crypto/sha1"
	"encoding/base64"
	"encoding/binary"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/url"
	"strings"
	"time"
)

const (
	OpContinuation byte = 0x0
	OpText         byte = 0x1
	OpBinary       byte = 0x2
	OpClose        byte = 0x8
	OpPing         byte = 0x9
	OpPong         byte = 0xA
)

// Client is a connected WebSocket client.
type Client struct {
	conn net.Conn
	br   *bufio.Reader
}

// Dial connects to a ws:// URL and performs the handshake.
func Dial(rawURL string) (*Client, error) {
	u, err := url.Parse(rawURL)
	if err != nil {
		return nil, err
	}
	host := u.Host
	if u.Port() == "" {
		host += ":80"
	}
	conn, err := net.DialTimeout("tcp", host, 5*time.Second)
	if err != nil {
		return nil, err
	}

	var keyBytes [16]byte
	if _, err := rand.Read(keyBytes[:]); err != nil {
		conn.Close()
		return nil, err
	}
	key := base64.StdEncoding.EncodeToString(keyBytes[:])

	path := u.RequestURI()
	if path == "" {
		path = "/"
	}
	req := fmt.Sprintf("GET %s HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"+
		"Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n", path, u.Host, key)
	if _, err := conn.Write([]byte(req)); err != nil {
		conn.Close()
		return nil, err
	}

	br := bufio.NewReader(conn)
	resp, err := http.ReadResponse(br, nil)
	if err != nil {
		conn.Close()
		return nil, err
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusSwitchingProtocols {
		conn.Close()
		return nil, fmt.Errorf("handshake: %s", resp.Status)
	}
	sum := sha1.Sum([]byte(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11")) // #nosec G401 — the RFC asks for it
	if got := resp.Header.Get("Sec-WebSocket-Accept"); got != base64.StdEncoding.EncodeToString(sum[:]) {
		conn.Close()
		return nil, fmt.Errorf("handshake: bad accept key %q", got)
	}
	return &Client{conn: conn, br: br}, nil
}

func (c *Client) writeFrame(opcode byte, fin bool, payload []byte) error {
	first := opcode
	if fin {
		first |= 0x80
	}
	hdr := []byte{first, 0x80} // masked, client to server
	switch n := len(payload); {
	case n < 126:
		hdr[1] |= byte(n)
	case n <= 0xFFFF:
		hdr[1] |= 126
		hdr = append(hdr, byte(n>>8), byte(n))
	default:
		hdr[1] |= 127
		var b [8]byte
		binary.BigEndian.PutUint64(b[:], uint64(n))
		hdr = append(hdr, b[:]...)
	}
	var mask [4]byte
	if _, err := rand.Read(mask[:]); err != nil {
		return err
	}
	hdr = append(hdr, mask[:]...)
	masked := make([]byte, len(payload))
	for i := range payload {
		masked[i] = payload[i] ^ mask[i%4]
	}
	_, err := c.conn.Write(append(hdr, masked...))
	return err
}

// WriteText sends one text message.
func (c *Client) WriteText(s string) error { return c.writeFrame(OpText, true, []byte(s)) }

// WriteFrameRaw sends a frame exactly as told, so a test can build invalid input.
func (c *Client) WriteFrameRaw(opcode byte, fin bool, payload []byte) error {
	return c.writeFrame(opcode, fin, payload)
}

// WriteFrameUnmasked sends a frame without the mask bit, which a server must reject.
func (c *Client) WriteFrameUnmasked(opcode byte, payload []byte) error {
	first := opcode | 0x80
	hdr := []byte{first, byte(len(payload))} // no mask bit
	_, err := c.conn.Write(append(hdr, payload...))
	return err
}

// ReadMessage returns one complete message, assembling fragments. Control frames come back
// as themselves.
func (c *Client) ReadMessage() (byte, []byte, error) {
	var msg []byte
	var op byte
	for {
		var hdr [2]byte
		if _, err := io.ReadFull(c.br, hdr[:]); err != nil {
			return 0, nil, err
		}
		fin := hdr[0]&0x80 != 0
		code := hdr[0] & 0x0F
		masked := hdr[1]&0x80 != 0
		length := uint64(hdr[1] & 0x7F)
		switch length {
		case 126:
			var b [2]byte
			if _, err := io.ReadFull(c.br, b[:]); err != nil {
				return 0, nil, err
			}
			length = uint64(binary.BigEndian.Uint16(b[:]))
		case 127:
			var b [8]byte
			if _, err := io.ReadFull(c.br, b[:]); err != nil {
				return 0, nil, err
			}
			length = binary.BigEndian.Uint64(b[:])
		}
		var mask [4]byte
		if masked {
			if _, err := io.ReadFull(c.br, mask[:]); err != nil {
				return 0, nil, err
			}
		}
		payload := make([]byte, length)
		if _, err := io.ReadFull(c.br, payload); err != nil {
			return 0, nil, err
		}
		if masked {
			for i := range payload {
				payload[i] ^= mask[i%4]
			}
		}

		if code >= 0x8 {
			return code, payload, nil
		}
		switch code {
		case OpContinuation:
			msg = append(msg, payload...)
		default:
			op = code
			msg = append(msg, payload...)
		}
		if fin {
			return op, msg, nil
		}
	}
}

// SetReadDeadline bounds how long the next read waits.
func (c *Client) SetReadDeadline(t time.Time) { _ = c.conn.SetReadDeadline(t) }

// CloseCode parses a close frame's payload.
func CloseCode(payload []byte) (uint16, string) {
	if len(payload) < 2 {
		return 0, ""
	}
	return binary.BigEndian.Uint16(payload[:2]), strings.TrimSpace(string(payload[2:]))
}

// Close closes the underlying socket.
func (c *Client) Close() error { return c.conn.Close() }
