// Relay protocol, the same one the Node relay (app/tools/relay-server.ts) speaks, so the
// firmware and the app need no changes to talk to this server.
//
//	!auth:<secret>   tunnel → relay, first message
//	!ok              relay → tunnel, auth accepted
//	!open:<cid>      relay → tunnel, a client connected
//	!ready:<cid>     tunnel → relay, the client may be forwarded to
//	!close:<cid>     either direction, the connection is gone
//	!error:<msg>     relay → tunnel
//	<cid>:<payload>  either direction, an app-protocol frame for client <cid>
//
// The relay is transparent: it never looks inside a payload, and the hub's own token auth
// runs end to end.
package main

import (
	"crypto/subtle"
	"fmt"
	"io"
	"net/http"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/kliuchnikovv/keystone-esp/relay/internal/ws"
)

type client struct {
	cid   int
	conn  *ws.Conn
	hub   *hub
	mu    sync.Mutex
	ready bool
	// Frames the client sent before the hub reported !ready: forwarded only after that,
	// so the hub does not see a hello before it has the client in its table.
	pending [][]byte
	gone    bool
}

type hub struct {
	id      string
	tunnel  *ws.Conn
	clients map[int]*client
	nextCid int

	mu sync.Mutex
}

type registry struct {
	mu   sync.Mutex
	hubs map[string]*hub
}

func newRegistry() *registry { return &registry{hubs: make(map[string]*hub)} }

func (r *registry) get(id string) *hub {
	r.mu.Lock()
	defer r.mu.Unlock()
	return r.hubs[id]
}

// attach registers a tunnel. A second tunnel for the same id replaces the first one: a hub
// that reconnected must find its clients gone, not split across two sockets.
func (r *registry) attach(id string, conn *ws.Conn) (*hub, []int) {
	r.mu.Lock()
	old := r.hubs[id]
	h := &hub{id: id, tunnel: conn, clients: make(map[int]*client), nextCid: 1}
	r.hubs[id] = h
	r.mu.Unlock()

	var dropped []int
	if old != nil {
		old.mu.Lock()
		for cid := range old.clients {
			dropped = append(dropped, cid)
		}
		old.mu.Unlock()
		for _, cid := range dropped {
			_ = old.sendTo(cid, nil, ws.CloseGoingAway, "tunnel replaced")
		}
		_ = old.tunnel.WriteClose(ws.CloseGoingAway, "tunnel replaced")
	}
	return h, dropped
}

func (r *registry) detach(id string, conn *ws.Conn) {
	r.mu.Lock()
	h := r.hubs[id]
	if h == nil || h.tunnel != conn {
		r.mu.Unlock()
		return
	}
	delete(r.hubs, id)
	r.mu.Unlock()

	h.mu.Lock()
	cids := make([]int, 0, len(h.clients))
	for cid := range h.clients {
		cids = append(cids, cid)
	}
	h.mu.Unlock()
	for _, cid := range cids {
		_ = h.sendTo(cid, nil, ws.CloseGoingAway, "hub disconnected")
	}
}

func (h *hub) clientCount() int {
	h.mu.Lock()
	defer h.mu.Unlock()
	return len(h.clients)
}

// addClient registers a remote client and tells the hub about it. It reports false when the
// hub is gone or already has as many clients as the backstop allows.
func (h *hub) addClient(conn *ws.Conn, maxClients int) (*client, bool, string) {
	h.mu.Lock()
	if len(h.clients) >= maxClients {
		h.mu.Unlock()
		return nil, false, "too many clients"
	}
	c := &client{cid: h.nextCid, conn: conn, hub: h}
	h.nextCid++
	h.clients[c.cid] = c
	h.mu.Unlock()

	// The hub answers !ready when the client is in its table; anything the client says
	// before that is held (see client.fromRemote).
	return c, true, ""
}

func (h *hub) dropClient(cid int) *client {
	h.mu.Lock()
	c := h.clients[cid]
	delete(h.clients, cid)
	h.mu.Unlock()
	return c
}

func (h *hub) sendTo(cid int, payload []byte, code uint16, reason string) error {
	h.mu.Lock()
	c := h.clients[cid]
	h.mu.Unlock()
	if c == nil {
		return nil
	}
	if payload == nil {
		return c.conn.WriteClose(code, reason)
	}
	msg := make([]byte, 0, len(payload)+8)
	msg = append(msg, []byte(strconv.Itoa(cid)+":")...)
	msg = append(msg, payload...)
	return c.conn.WriteText(msg)
}

func (h *hub) sendControl(msg string) {
	_ = h.tunnel.WriteText([]byte(msg))
}

// fromRemote is a frame the hub forwarded for client cid. It goes out at once: holding it
// until !ready is only right in the other direction. The hub answers a refused client with
// an error and then closes it, and that error never gets a !ready.
func (h *hub) fromRemote(cid int, opcode byte, payload []byte) bool {
	h.mu.Lock()
	c := h.clients[cid]
	h.mu.Unlock()
	if c == nil {
		return false
	}
	if opcode != ws.OpBinary {
		opcode = ws.OpText
	}
	return c.conn.WriteMessage(opcode, payload) == nil
}

// toRemote is a frame a client sent; it is held until the hub reports !ready.
func (c *client) toRemote(data []byte) {
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.gone {
		return
	}
	if c.ready {
		msg := make([]byte, 0, len(data)+8)
		msg = append(msg, []byte(strconv.Itoa(c.cid)+":")...)
		msg = append(msg, data...)
		_ = c.hub.tunnel.WriteText(msg)
		return
	}
	c.pending = append(c.pending, data)
}

// flushRemote hands over everything the client said before !ready.
func (c *client) flushRemote() {
	c.mu.Lock()
	pending := c.pending
	c.pending = nil
	c.ready = true
	c.mu.Unlock()
	for _, p := range pending {
		msg := make([]byte, 0, len(p)+8)
		msg = append(msg, []byte(strconv.Itoa(c.cid)+":")...)
		msg = append(msg, p...)
		_ = c.hub.tunnel.WriteText(msg)
	}
}

func (c *client) markGone() {
	c.mu.Lock()
	c.gone = true
	c.pending = nil
	c.mu.Unlock()
}

// parseCid reads "<cid>:" and returns the id and the rest.
func parseCid(msg []byte) (int, []byte, bool) {
	i := strings.IndexByte(string(msg), ':')
	if i <= 0 {
		return 0, nil, false
	}
	cid, err := strconv.Atoi(string(msg[:i]))
	if err != nil || cid <= 0 {
		return 0, nil, false
	}
	return cid, msg[i+1:], true
}

func prefixOf(msg []byte, p string) ([]byte, bool) {
	if len(msg) < len(p) || string(msg[:len(p)]) != p {
		return nil, false
	}
	return msg[len(p):], true
}

// handleTunnel serves /tunnel/<hub-id>: the hub dialling out.
func (s *server) handleTunnel(w http.ResponseWriter, r *http.Request, hubID string) {
	conn, err := ws.Upgrade(w, r, s.wsOpts)
	if err != nil {
		return
	}
	defer conn.Close()

	// The first message must be the shared secret, and it must be quick.
	_ = conn.SetReadDeadline(time.Now().Add(s.authTimeout))
	op, msg, err := conn.ReadMessage()
	if err != nil || op != ws.OpText {
		_ = conn.WriteClose(ws.ClosePolicy, "auth timeout")
		return
	}
	secret, ok := prefixOf(msg, "!auth:")
	if !ok || subtle.ConstantTimeCompare(secret, []byte(s.secret)) != 1 {
		logf("tunnel", "%s refused (bad secret from %s)", hubID, conn.RemoteAddr())
		_ = conn.WriteText([]byte("!error:unauthorized"))
		_ = conn.WriteClose(ws.ClosePolicy, "unauthorized")
		return
	}

	h, dropped := s.hubs.attach(hubID, conn)
	if len(dropped) > 0 {
		logf("tunnel", "%s replaced an older tunnel, dropped %d client(s)", hubID, len(dropped))
	}
	logf("tunnel", "%s registered from %s", hubID, conn.RemoteAddr())
	defer s.hubs.detach(hubID, conn)

	_ = conn.WriteText([]byte("!ok"))
	// Past the handshake the read deadline is owned by ReadMessage (ping interval + slack):
	// every pong resets it, so a relay that goes quiet past that is a dead connection.
	conn.Keepalive(s.pingInterval, func(err error) {
		logf("tunnel", "%s: %v", hubID, err)
	})

	for {
		op, msg, err := conn.ReadMessage()
		if err != nil {
			if !conn.Closed() && err != io.EOF {
				logf("tunnel", "%s: %v", hubID, err)
			}
			return
		}
		switch op {
		case ws.OpPing:
			_ = conn.WritePong(msg)
			continue
		case ws.OpClose:
			_ = conn.WriteClose(ws.CloseNormal, "")
			return
		case ws.OpPong:
			continue
		}
		if op != ws.OpText && op != ws.OpBinary {
			continue // control frames are handled above; nothing else may arrive
		}

		if rest, ok := prefixOf(msg, "!ready:"); ok {
			if cid, err := strconv.Atoi(string(rest)); err == nil {
				if cid > 0 {
					h.mu.Lock()
					c := h.clients[cid]
					h.mu.Unlock()
					if c != nil {
						c.flushRemote()
					}
				}
			}
			continue
		}
		if rest, ok := prefixOf(msg, "!close:"); ok {
			if cid, err := strconv.Atoi(string(rest)); err == nil && cid > 0 {
				if c := h.dropClient(cid); c != nil {
					logf("client", "%s#%d closed by the hub", hubID, cid)
					c.markGone()
					_ = c.conn.WriteClose(ws.CloseNormal, "hub closed the connection")
				}
			}
			continue
		}
		if len(msg) > 0 && msg[0] == '!' {
			logf("tunnel", "%s sent an unknown control frame %q", hubID, truncate(msg, 40))
			continue
		}
		cid, payload, ok := parseCid(msg)
		if !ok {
			logf("tunnel", "%s sent an unparsable frame %q", hubID, truncate(msg, 40))
			continue
		}
		if !h.fromRemote(cid, op, payload) {
			logf("client", "%s#%d dropped: frame for a client that is gone", hubID, cid)
		}
	}
}

// handleClient serves /client/<hub-id>: a phone or browser outside the LAN.
func (s *server) handleClient(w http.ResponseWriter, r *http.Request, hubID string) {
	h := s.hubs.get(hubID)
	if h == nil {
		http.Error(w, "hub not connected", http.StatusServiceUnavailable)
		return
	}
	conn, err := ws.Upgrade(w, r, s.wsOpts)
	if err != nil {
		return
	}
	defer conn.Close()

	c, ok, why := h.addClient(conn, s.maxClients)
	if !ok {
		logf("client", "%s refused: %s", hubID, why)
		_ = conn.WriteClose(ws.CloseTooManyUsers, why)
		return
	}
	h.sendControl(fmt.Sprintf("!open:%d", c.cid))
	logf("client", "%s#%d connected from %s (%d total)", hubID, c.cid, conn.RemoteAddr(), h.clientCount())

	conn.Keepalive(s.pingInterval, func(err error) {
		logf("client", "%s#%d: %v", hubID, c.cid, err)
	})
	defer func() {
		c.markGone()
		if old := h.dropClient(c.cid); old != nil {
			h.sendControl(fmt.Sprintf("!close:%d", c.cid))
			logf("client", "%s#%d disconnected", hubID, c.cid)
		}
	}()

	for {
		op, msg, err := conn.ReadMessage()
		if err != nil {
			if !conn.Closed() && err != io.EOF {
				logf("client", "%s#%d: %v", hubID, c.cid, err)
			}
			return
		}
		switch op {
		case ws.OpPing:
			_ = conn.WritePong(msg)
			continue
		case ws.OpClose:
			return
		case ws.OpPong:
			continue
		}
		if op != ws.OpText && op != ws.OpBinary {
			continue
		}
		// Forwarded as text with the cid in front — the hub's side of the protocol is text
		// only, and the hub answers a binary frame itself (bad_json, then it hangs up).
		c.toRemote(msg)
	}
}

func truncate(b []byte, n int) string {
	if len(b) <= n {
		return string(b)
	}
	return string(b[:n]) + "…"
}
