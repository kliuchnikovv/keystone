// Keystone Relay — the security policy.
//
// The relay is a trusted component: it sees the hub's token and the traffic inside
// the tunnel (docs/relay-security.md). That makes these rules the whole defence of
// a publicly reachable relay — a wrong secret must not be guessable, a stranger
// must not be able to enumerate the hubs, and nobody must be able to exhaust the
// memory of the process.
package main

import (
	"crypto/sha256"
	"crypto/subtle"
	"net"
	"net/url"
	"regexp"
	"strings"
	"sync"
	"time"
)

const (
	// MaxFrameBytes is one protocol frame. The app protocol splits its snapshot
	// into 4 KB parts, so 64 KB leaves room without inviting a memory hog.
	MaxFrameBytes = 64 * 1024
	// MaxBufferedFrames and MaxBufferedBytes bound what may wait for the tunnel
	// to open its local connection to the hub.
	MaxBufferedFrames = 64
	MaxBufferedBytes  = 256 * 1024
	// MaxHubs is how many hubs this relay serves.
	MaxHubs = 64
	// MaxClientsPerHub is how many remote clients one hub may have at a time.
	MaxClientsPerHub = 3
	// AuthFailures before a source is blocked; AuthWindow is how long the
	// failures are counted, AuthBlock how long a blocked source waits.
	AuthFailures = 5
	AuthWindow   = time.Minute
	AuthBlock    = time.Minute
)

var hubIDPattern = regexp.MustCompile(`^[A-Za-z0-9][A-Za-z0-9_-]{0,63}$`)

// SecretMatches compares two secrets without leaking their length or their
// first difference. Both sides are hashed first, so the comparison always gets
// buffers of the same size.
func SecretMatches(a, b string) bool {
	ha := sha256.Sum256([]byte(a))
	hb := sha256.Sum256([]byte(b))
	return subtle.ConstantTimeCompare(ha[:], hb[:]) == 1
}

// HubIDValid keeps a hub id boring: it travels in a URL path, into the logs
// and into the status page. No slashes, no spaces, no control characters.
func HubIDValid(id string) bool {
	return hubIDPattern.MatchString(id)
}

// OriginAllowed decides which origins may open a socket to the relay.
//
// A browser always sends an Origin, and a website in a browser must not be able
// to spend a client's slot on the hub, so a foreign origin is refused unless it
// is listed explicitly (RELAY_ALLOWED_ORIGINS).
//
// A native client is a different case. React Native's WebSocket on iOS is
// SocketRocket, and it mirrors the address it dials into the Origin header
// ("wss" becomes "https") — the phone app therefore sends the relay's own
// origin. That is the same origin, not a foreign site, and it is allowed: only
// the host has to match, because the scheme the relay sees is not the scheme
// the client used (TLS is terminated in front of this process).
func OriginAllowed(origin string, allowlist []string, reqHost string) bool {
	if origin == "" {
		return true
	}
	if sameHost(origin, reqHost) {
		return true
	}
	o := normalizeOrigin(origin)
	for _, a := range allowlist {
		if normalizeOrigin(a) == o {
			return true
		}
	}
	return false
}

// sameHost reports whether the origin points at the host the request came to.
// A page on another site cannot fake this: a browser sends the origin of the
// page it runs on, not the address of the socket it opens.
func sameHost(origin, reqHost string) bool {
	u, err := url.Parse(origin)
	if err != nil || u.Scheme == "" || u.Host == "" {
		return false
	}
	host, _, err := net.SplitHostPort(reqHost)
	if err != nil {
		host = reqHost
	}
	return host != "" && strings.EqualFold(u.Hostname(), host)
}

// normalizeOrigin puts an origin into one shape: lower case, no spaces, no
// trailing slash. A browser sends "https://app.example", a person writes
// "https://app.example/" — the trailing slash must not make them differ.
func normalizeOrigin(s string) string {
	s = strings.ToLower(strings.TrimSpace(s))
	return strings.TrimRight(s, "/")
}

// ParseOriginAllowlist reads the RELAY_ALLOWED_ORIGINS value: a comma-separated list.
func ParseOriginAllowlist(value string) []string {
	if value == "" {
		return nil
	}
	var out []string
	for _, part := range strings.Split(value, ",") {
		if p := strings.TrimSpace(part); p != "" {
			out = append(out, p)
		}
	}
	return out
}

type failEntry struct {
	count        int
	first        time.Time
	blockedUntil time.Time
}

// AuthLimiter counts failed authentications per source and blocks it for a
// while. Without this, the shared secret is only as strong as the patience of
// whoever is guessing it — the relay has no lockout of its own.
type AuthLimiter struct {
	mu          sync.Mutex
	fails       map[string]*failEntry
	maxFailures int
	window      time.Duration
	block       time.Duration
}

// NewAuthLimiter makes a limiter with the given rules.
func NewAuthLimiter(maxFailures int, window, block time.Duration) *AuthLimiter {
	return &AuthLimiter{
		fails:       make(map[string]*failEntry),
		maxFailures: maxFailures,
		window:      window,
		block:       block,
	}
}

// Blocked reports whether the source is currently blocked.
func (l *AuthLimiter) Blocked(key string, now time.Time) bool {
	l.mu.Lock()
	defer l.mu.Unlock()
	e := l.fails[key]
	if e == nil {
		return false
	}
	if e.blockedUntil.After(now) {
		return true
	}
	if !e.blockedUntil.IsZero() {
		// The block expired: start counting again.
		e.blockedUntil = time.Time{}
		e.count = 0
		e.first = now
	}
	return false
}

// Fail counts one failure; the source is blocked on the N-th failure.
func (l *AuthLimiter) Fail(key string, now time.Time) {
	l.mu.Lock()
	defer l.mu.Unlock()
	e := l.fails[key]
	if e == nil || now.Sub(e.first) > l.window {
		// A new counting window.
		e = &failEntry{}
		l.fails[key] = e
		e.first = now
	}
	e.count++
	if e.count >= l.maxFailures {
		e.blockedUntil = now.Add(l.block)
		e.count = 0
		e.first = now
	}
}

// Success clears the counter of a source that authenticated.
func (l *AuthLimiter) Success(key string) {
	l.mu.Lock()
	defer l.mu.Unlock()
	delete(l.fails, key)
}

// Sweep drops the entries that are neither counted nor blocked any more.
func (l *AuthLimiter) Sweep(now time.Time) {
	l.mu.Lock()
	defer l.mu.Unlock()
	for key, e := range l.fails {
		if !e.blockedUntil.After(now) && now.Sub(e.first) > l.window {
			delete(l.fails, key)
		}
	}
}

// StatusPageBody renders the status page. The status page of a public relay
// must not be a directory of the homes on it: without a token only the
// counters are shown.
//
// With a token the page also says where the app should point for each hub:
// a person copies an address from here, and the relay's own home page is not
// the address the app talks to.
func StatusPageBody(hubIDs []string, clientCounts map[string]int, idsAllowed bool, host string) string {
	var b strings.Builder
	b.WriteString("Keystone Relay\n")
	b.WriteString(itoa(len(hubIDs)) + " hub(s) connected")
	if !idsAllowed {
		b.WriteString("\n")
		return b.String()
	}
	for _, id := range hubIDs {
		b.WriteString("\n  " + id + ": " + itoa(clientCounts[id]) + " client(s)")
		if host != "" {
			b.WriteString("\n      app address: wss://" + host + "/client/" + id)
		}
	}
	b.WriteString("\n")
	return b.String()
}

// StatusTokenOK reports whether the request carries the status token.
func StatusTokenOK(provided, expected string) bool {
	if expected == "" {
		// No token configured: the caller shows counters only.
		return true
	}
	if provided == "" {
		return false
	}
	return SecretMatches(provided, expected)
}

func itoa(n int) string {
	if n == 0 {
		return "0"
	}
	neg := n < 0
	if neg {
		n = -n
	}
	var buf [20]byte
	i := len(buf)
	for n > 0 {
		i--
		buf[i] = byte('0' + n%10)
		n /= 10
	}
	if neg {
		i--
		buf[i] = '-'
	}
	return string(buf[i:])
}
