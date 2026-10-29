// Keystone Relay — the server.
//
// The wire protocol between the relay and a tunnel (text WebSocket frames):
//
//	!auth:<secret>   tunnel → relay, first message
//	!ok              relay → tunnel, auth accepted
//	!open:<cid>      relay → tunnel, a client connected — open a local hub connection
//	!ready:<cid>     tunnel → relay, local connection open, start forwarding
//	!close:<cid>     either direction, connection gone
//	!error:<msg>     relay → tunnel
//	<cid>:<payload>  either direction, a hub-protocol text frame for client <cid>
//
// This protocol is spoken by the firmware of the hub (firmware/brain-s3/main/relay_client.cpp),
// so it must not change: the running hubs talk to this relay as it is.
package main

import (
	"context"
	"fmt"
	"io"
	"net"
	"net/http"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/coder/websocket"
)

// conn is a WebSocket with a write lock: several goroutines may want to write
// to the same connection (the tunnel's own reader forwards frames, and a
// client handler asks the tunnel to open a connection).
type conn struct {
	c  *websocket.Conn
	mu sync.Mutex
}

func (c *conn) write(ctx context.Context, msg string) error {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.c.Write(ctx, websocket.MessageText, []byte(msg))
}

func (c *conn) close(status websocket.StatusCode, reason string) {
	_ = c.c.Close(status, reason)
}

type buffered struct {
	frames []string
	bytes  int
}

type hub struct {
	mu       sync.Mutex
	id       string
	tunnel   *conn
	clients  map[int64]*conn
	nextCid  int64
	buffered map[int64]*buffered
}

// addClient registers a remote client and returns its id and the tunnel to talk to.
func (h *hub) addClient(c *conn, maxClients int) (int64, *conn, bool) {
	h.mu.Lock()
	defer h.mu.Unlock()
	if len(h.clients) >= maxClients {
		return 0, nil, false
	}
	cid := h.nextCid
	h.nextCid++
	h.clients[cid] = c
	h.buffered[cid] = &buffered{}
	return cid, h.tunnel, true
}

func (h *hub) getClient(cid int64) *conn {
	h.mu.Lock()
	defer h.mu.Unlock()
	return h.clients[cid]
}

func (h *hub) takeClient(cid int64) *conn {
	h.mu.Lock()
	defer h.mu.Unlock()
	c := h.clients[cid]
	delete(h.clients, cid)
	delete(h.buffered, cid)
	return c
}

// addFrame holds a frame while the tunnel is still opening its local connection.
// It reports whether the frame must go to the tunnel right away, and whether the
// buffer limit is exceeded.
func (h *hub) addFrame(cid int64, msg string) (forward bool, over bool) {
	h.mu.Lock()
	defer h.mu.Unlock()
	b := h.buffered[cid]
	if b == nil {
		return true, false
	}
	b.frames = append(b.frames, msg)
	b.bytes += len(msg)
	if len(b.frames) > MaxBufferedFrames || b.bytes > MaxBufferedBytes {
		return false, true
	}
	return false, false
}

// takeBuffered hands over the frames that waited for the hub and stops buffering.
func (h *hub) takeBuffered(cid int64) []string {
	h.mu.Lock()
	defer h.mu.Unlock()
	b := h.buffered[cid]
	if b == nil {
		return nil
	}
	delete(h.buffered, cid)
	return b.frames
}

func (h *hub) snapshotClients() []*conn {
	h.mu.Lock()
	defer h.mu.Unlock()
	out := make([]*conn, 0, len(h.clients))
	for _, c := range h.clients {
		out = append(out, c)
	}
	return out
}

type relay struct {
	secret         string
	statusToken    string
	allowedOrigins []string
	maxClients     int
	maxHubs        int
	limiter        *AuthLimiter

	mu   sync.Mutex
	hubs map[string]*hub
}

func newRelay(secret, statusToken string, allowedOrigins []string, maxClients, maxHubs int) *relay {
	return &relay{
		secret:         secret,
		statusToken:    statusToken,
		allowedOrigins: allowedOrigins,
		maxClients:     maxClients,
		maxHubs:        maxHubs,
		limiter:        NewAuthLimiter(AuthFailures, AuthWindow, AuthBlock),
		hubs:           make(map[string]*hub),
	}
}

func (r *relay) log(tag, msg string) {
	fmt.Printf("%s  [%s] %s\n", time.Now().Format("15:04:05"), tag, msg)
}

func (r *relay) getHub(id string) *hub {
	r.mu.Lock()
	defer r.mu.Unlock()
	return r.hubs[id]
}

// registerTunnel puts a tunnel in place. A second tunnel under the same id
// takes over — with a shared secret that is also what a takeover looks like,
// so it is worth a loud line in the log.
func (r *relay) registerTunnel(c *conn, hubID string) bool {
	r.mu.Lock()
	if _, exists := r.hubs[hubID]; !exists && len(r.hubs) >= r.maxHubs {
		r.mu.Unlock()
		return false
	}
	old := r.hubs[hubID]
	h := &hub{
		id:       hubID,
		tunnel:   c,
		clients:  make(map[int64]*conn),
		nextCid:  1,
		buffered: make(map[int64]*buffered),
	}
	r.hubs[hubID] = h
	r.mu.Unlock()

	if old != nil {
		r.log("tunnel", hubID+" REPLACED by another tunnel")
		for _, cws := range old.snapshotClients() {
			cws.close(websocket.StatusGoingAway, "tunnel replaced")
		}
		old.tunnel.close(websocket.StatusGoingAway, "replaced")
	}
	return true
}

// dropHub removes the hub if it is still the one that registered it.
func (r *relay) dropHub(hubID string, h *hub) {
	r.mu.Lock()
	defer r.mu.Unlock()
	if r.hubs[hubID] == h {
		delete(r.hubs, hubID)
	}
}

func (r *relay) snapshotHubs() ([]string, map[string]int) {
	r.mu.Lock()
	defer r.mu.Unlock()
	ids := make([]string, 0, len(r.hubs))
	counts := make(map[string]int, len(r.hubs))
	for id, h := range r.hubs {
		ids = append(ids, id)
		h.mu.Lock()
		counts[id] = len(h.clients)
		h.mu.Unlock()
	}
	return ids, counts
}

// ServeHTTP routes both plain requests and WebSocket upgrades.
func (r *relay) ServeHTTP(w http.ResponseWriter, req *http.Request) {
	path := strings.Trim(req.URL.Path, "/")

	// The health check: it must answer without a token and must not say anything
	// about the hubs on this relay.
	if path == "healthz" {
		w.Header().Set("Content-Type", "text/plain; charset=utf-8")
		w.WriteHeader(http.StatusOK)
		_, _ = io.WriteString(w, "ok\n")
		return
	}

	if path == "" {
		if isUpgrade(req) {
			http.Error(w, "expected a path", http.StatusBadRequest)
			return
		}
		r.status(w, req)
		return
	}

	parts := strings.Split(path, "/")
	if len(parts) != 2 || (parts[0] != "tunnel" && parts[0] != "client") || !HubIDValid(parts[1]) {
		http.NotFound(w, req)
		return
	}
	if !isUpgrade(req) {
		http.Error(w, "expected a websocket", http.StatusBadRequest)
		return
	}
	// Native clients (the hub, the phone app, the console) send no Origin header.
	// A browser always does, and a website must not be able to spend a client's
	// slot on the hub, so browser origins are refused unless listed explicitly.
	if !OriginAllowed(req.Header.Get("Origin"), r.allowedOrigins) {
		origin := req.Header.Get("Origin")
		r.log("relay", fmt.Sprintf("refused origin %q from %s", origin, clientIP(req)))
		http.Error(w, fmt.Sprintf("forbidden: origin %q is refused unless RELAY_ALLOWED_ORIGINS lists it", origin), http.StatusForbidden)
		return
	}
	if parts[0] == "tunnel" {
		r.handleTunnel(w, req, parts[1], clientIP(req))
		return
	}
	r.handleClient(w, req, parts[1])
}

// status serves the status page. The status page of a public relay must not be
// a directory of the homes on it: with RELAY_STATUS_TOKEN set, the hub ids are
// shown to whoever holds the token; without it, only the counters are shown.
func (r *relay) status(w http.ResponseWriter, req *http.Request) {
	provided := ""
	if h := req.Header.Get("Authorization"); strings.HasPrefix(h, "Bearer ") {
		provided = strings.TrimSpace(h[len("Bearer "):])
	} else if t := req.URL.Query().Get("token"); t != "" {
		provided = t
	}
	if r.statusToken != "" && !StatusTokenOK(provided, r.statusToken) {
		w.Header().Set("WWW-Authenticate", "Bearer")
		http.Error(w, "unauthorized", http.StatusUnauthorized)
		return
	}
	ids, counts := r.snapshotHubs()
	w.Header().Set("Content-Type", "text/plain; charset=utf-8")
	_, _ = io.WriteString(w, StatusPageBody(ids, counts, r.statusToken != "", req.Host))
}

func (r *relay) handleTunnel(w http.ResponseWriter, req *http.Request, hubID, ip string) {
	c, err := websocket.Accept(w, req, &websocket.AcceptOptions{InsecureSkipVerify: true})
	if err != nil {
		return
	}
	conn := &conn{c: c}
	ctx := req.Context()
	defer conn.close(websocket.StatusNormalClosure, "")
	c.SetReadLimit(MaxFrameBytes)

	stop := make(chan struct{})
	defer close(stop)
	go func() {
		t := time.NewTicker(30 * time.Second)
		defer t.Stop()
		for {
			select {
			case <-stop:
				return
			case <-t.C:
				pingCtx, cancel := context.WithTimeout(ctx, 10*time.Second)
				_ = c.Ping(pingCtx)
				cancel()
			}
		}
	}()

	authed := false
	for {
		typ, data, err := c.Read(ctx)
		if err != nil {
			break
		}
		if typ != websocket.MessageText {
			continue
		}
		msg := string(data)

		if !authed {
			now := time.Now()
			if r.limiter.Blocked(ip, now) {
				_ = conn.write(ctx, "!error:rate limited")
				conn.close(websocket.StatusPolicyViolation, "rate limited")
				break
			}
			if strings.HasPrefix(msg, "!auth:") && SecretMatches(msg[len("!auth:"):], r.secret) {
				r.limiter.Success(ip)
				if !r.registerTunnel(conn, hubID) {
					_ = conn.write(ctx, "!error:too many hubs")
					conn.close(websocket.StatusTryAgainLater, "too many hubs")
					break
				}
				authed = true
				_ = conn.write(ctx, "!ok")
				r.log("tunnel", hubID+" registered")
				continue
			}
			r.limiter.Fail(ip, now)
			_ = conn.write(ctx, "!error:unauthorized")
			conn.close(websocket.StatusPolicyViolation, "unauthorized")
			break
		}

		h := r.getHub(hubID)
		if h == nil || h.tunnel != conn {
			break
		}
		switch {
		case strings.HasPrefix(msg, "!ready:"):
			cid := parseCID(msg[len("!ready:"):])
			// The frames that waited for the hub carry the client's id too: the
			// tunnel speaks "<cid>:<payload>", and a frame without it is dropped
			// by the firmware. The hello of a client always lands here - the app
			// sends it the moment the socket opens, long before the hub answers.
			for _, frame := range h.takeBuffered(cid) {
				_ = conn.write(ctx, fmt.Sprintf("%d:%s", cid, frame))
			}
		case strings.HasPrefix(msg, "!close:"):
			cid := parseCID(msg[len("!close:"):])
			if cws := h.takeClient(cid); cws != nil {
				cws.close(websocket.StatusNormalClosure, "hub disconnected")
			}
		default:
			if idx := strings.Index(msg, ":"); idx > 0 {
				cid := parseCID(msg[:idx])
				payload := msg[idx+1:]
				if cws := h.getClient(cid); cws != nil {
					_ = cws.write(ctx, payload)
				}
			}
		}
	}

	if h := r.getHub(hubID); h != nil && h.tunnel == conn {
		r.log("tunnel", fmt.Sprintf("%s disconnected (%d client(s) dropped)", hubID, len(h.snapshotClients())))
		for _, cws := range h.snapshotClients() {
			cws.close(websocket.StatusGoingAway, "tunnel lost")
		}
		r.dropHub(hubID, h)
	}
}

func (r *relay) handleClient(w http.ResponseWriter, req *http.Request, hubID string) {
	c, err := websocket.Accept(w, req, &websocket.AcceptOptions{InsecureSkipVerify: true})
	if err != nil {
		return
	}
	conn := &conn{c: c}
	ctx := req.Context()
	defer conn.close(websocket.StatusNormalClosure, "")
	c.SetReadLimit(MaxFrameBytes)

	h := r.getHub(hubID)
	if h == nil {
		conn.close(websocket.StatusInternalError, "hub not connected")
		return
	}
	cid, tunnel, ok := h.addClient(conn, r.maxClients)
	if !ok {
		conn.close(websocket.StatusTryAgainLater, "too many clients")
		return
	}
	_ = tunnel.write(ctx, fmt.Sprintf("!open:%d", cid))
	r.log("client", fmt.Sprintf("%s#%d connected", hubID, cid))

	for {
		typ, data, err := c.Read(ctx)
		if err != nil {
			break
		}
		if typ != websocket.MessageText {
			continue
		}
		msg := string(data)
		forward, over := h.addFrame(cid, msg)
		if over {
			r.log("client", fmt.Sprintf("%s#%d sent too much before the hub was ready — dropping", hubID, cid))
			conn.close(websocket.StatusMessageTooBig, "too much buffered")
			h.takeClient(cid)
			_ = tunnel.write(ctx, fmt.Sprintf("!close:%d", cid))
			return
		}
		if forward {
			_ = tunnel.write(ctx, fmt.Sprintf("%d:%s", cid, msg))
		}
	}

	h.takeClient(cid)
	_ = tunnel.write(ctx, fmt.Sprintf("!close:%d", cid))
	r.log("client", fmt.Sprintf("%s#%d disconnected", hubID, cid))
}

func parseCID(s string) int64 {
	n, err := strconv.ParseInt(strings.TrimSpace(s), 10, 64)
	if err != nil || n <= 0 {
		return 0
	}
	return n
}

func clientIP(req *http.Request) string {
	host, _, err := net.SplitHostPort(req.RemoteAddr)
	if err != nil {
		return req.RemoteAddr
	}
	return host
}

// isUpgrade reports whether the request asks for a WebSocket handshake.
func isUpgrade(req *http.Request) bool {
	return strings.EqualFold(req.Header.Get("Upgrade"), "websocket")
}
