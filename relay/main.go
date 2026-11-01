// Keystone relay — remote access to the hub without anything else in the house.
//
// The hub opens one WebSocket to this server and keeps it; a phone or browser opens another
// and the two are paired. The relay never reads the payloads, so the hub's own token auth
// stays end to end.
//
//	keystone-relay --secret <shared-secret> [--port 9090] [--tls-cert cert.pem --tls-key key.pem]
//
// Same protocol as app/tools/relay-server.ts (docs/app-protocol.md §13), so a hub configured
// against one works against the other. Single static binary, standard library only.
package main

import (
	"crypto/tls"
	"flag"
	"fmt"
	"log"
	"net/http"
	"os"
	"os/signal"
	"sort"
	"strings"
	"sync"
	"syscall"
	"time"

	"github.com/kliuchnikovv/keystone-esp/relay/internal/ws"
)

type server struct {
	secret       string
	hubs         *registry
	maxClients   int
	pingInterval time.Duration
	pingSlack    time.Duration
	authTimeout  time.Duration
	wsOpts       ws.Options
}

var logMu sync.Mutex

// logf writes one line in the same shape as the Node relay (app/tools/relay-server.ts):
// a timestamp, a tag, the message. The standard logger already stamps the time.
func logf(tag, format string, args ...any) {
	logMu.Lock()
	defer logMu.Unlock()
	log.Printf("[%s] %s", tag, fmt.Sprintf(format, args...))
}

// envOr returns the environment value for name, or def when it is unset or empty. Flags win
// over the environment.
func envOr(name, def string) string {
	if v := strings.TrimSpace(os.Getenv(name)); v != "" {
		return v
	}
	return def
}

func main() {
	var (
		port        = flag.Int("port", 9090, "TCP port to listen on")
		secret      = flag.String("secret", "", "shared secret the hub authenticates with (or $SECRET)")
		tlsCert     = flag.String("tls-cert", "", "TLS certificate; serve plain ws:// without it")
		tlsKey      = flag.String("tls-key", "", "TLS private key")
		maxClients  = flag.Int("max-clients", 16, "backstop against a flood; the hub itself allows 3 authenticated clients and answers \"busy\"")
		pingEvery   = flag.Duration("ping-interval", 30*time.Second, "keepalive ping interval (0 disables)")
		authTimeout = flag.Duration("auth-timeout", 10*time.Second, "how long a tunnel has to send !auth")
		maxMessage  = flag.Int("max-message", 1<<20, "largest accepted WebSocket message")
		readTimeout = flag.Duration("read-timeout", 0, "drop a connection that sends nothing for this long (0 = two ping intervals + 10 s)")
		showVersion = flag.Bool("version", false, "print the version and exit")
	)
	flag.Parse()

	if *showVersion {
		fmt.Println("keystone-relay", version)
		return
	}

	sec := *secret
	if sec == "" {
		sec = strings.TrimSpace(os.Getenv("SECRET"))
	}
	if sec == "" {
		fmt.Fprintln(os.Stderr, "keystone-relay: --secret (or $SECRET) is required")
		flag.Usage()
		os.Exit(2)
	}
	if len(sec) < 16 {
		fmt.Fprintln(os.Stderr, "keystone-relay: use a secret of at least 16 characters (openssl rand -hex 32)")
		os.Exit(2)
	}
	if (*tlsCert == "") != (*tlsKey == "") {
		fmt.Fprintln(os.Stderr, "keystone-relay: --tls-cert and --tls-key go together")
		os.Exit(2)
	}

	s := &server{
		secret:       sec,
		hubs:         newRegistry(),
		maxClients:   *maxClients,
		pingInterval: *pingEvery,
		authTimeout:  *authTimeout,
		wsOpts:       ws.Options{MaxMessage: *maxMessage, ReadTimeout: readTimeoutFor(*readTimeout, *pingEvery)},
	}

	mux := http.NewServeMux()
	mux.HandleFunc("/tunnel/", func(w http.ResponseWriter, r *http.Request) {
		s.handleTunnel(w, r, pathID(r.URL.Path, "tunnel"))
	})
	mux.HandleFunc("/client/", func(w http.ResponseWriter, r *http.Request) {
		s.handleClient(w, r, pathID(r.URL.Path, "client"))
	})
	mux.HandleFunc("/", s.statusPage)

	addr := fmt.Sprintf(":%d", *port)
	srv := &http.Server{
		Addr:              addr,
		Handler:           mux,
		ReadHeaderTimeout: 10 * time.Second,
		// No write timeout: WebSocket connections are long-lived and idle between events.
		ErrorLog: log.New(newLockedWriter(), "http ", 0),
	}

	scheme := "ws"
	if *tlsCert != "" {
		scheme = "wss"
	}
	logf("relay", "listening on %s (%s), max %d client(s) per hub, ping every %s",
		addr, scheme, *maxClients, *pingEvery)

	idle := make(chan struct{})
	go func() {
		sigs := make(chan os.Signal, 1)
		signal.Notify(sigs, os.Interrupt, syscall.SIGTERM)
		<-sigs
		logf("relay", "shutting down")
		_ = srv.Close()
		close(idle)
	}()

	var err error
	if *tlsCert != "" {
		cert, loadErr := tls.LoadX509KeyPair(*tlsCert, *tlsKey)
		if loadErr != nil {
			fmt.Fprintf(os.Stderr, "keystone-relay: %v\n", loadErr)
			os.Exit(1)
		}
		srv.TLSConfig = &tls.Config{
			Certificates: []tls.Certificate{cert},
			MinVersion:   tls.VersionTLS12,
			NextProtos:   []string{"http/1.1"},
		}
		err = srv.ListenAndServeTLS("", "")
	} else {
		err = srv.ListenAndServe()
	}
	if err != nil && err != http.ErrServerClosed {
		fmt.Fprintf(os.Stderr, "keystone-relay: %v\n", err)
		os.Exit(1)
	}
	<-idle
}

// readTimeoutFor picks how long a connection may stay silent before it is dropped. Every
// pong refreshes it, so it must survive one missed ping: a phone that slept through one is
// not a dead connection. Two ping intervals + 10 s catches a peer that really went away
// without punishing a slow one.
func readTimeoutFor(flagValue, pingInterval time.Duration) time.Duration {
	if flagValue > 0 {
		return flagValue
	}
	if pingInterval <= 0 {
		return 0 // keepalives off: no deadline, only the OS notices a dead socket
	}
	return 2*pingInterval + 10*time.Second
}

// pathID pulls "<kind>/<id>" apart and refuses anything else, so a bad path never turns into
// an empty hub id.
func pathID(path, kind string) string {
	parts := strings.Split(strings.Trim(path, "/"), "/")
	if len(parts) != 2 || parts[0] != kind || parts[1] == "" {
		return ""
	}
	return parts[1]
}

func (s *server) statusPage(w http.ResponseWriter, r *http.Request) {
	if r.URL.Path != "/" {
		http.NotFound(w, r)
		return
	}
	s.hubs.mu.Lock()
	names := make([]string, 0, len(s.hubs.hubs))
	for id := range s.hubs.hubs {
		names = append(names, id)
	}
	s.hubs.mu.Unlock()
	sort.Strings(names)

	var b strings.Builder
	b.WriteString("Keystone Relay\n")
	fmt.Fprintf(&b, "%d hub(s) connected\n", len(names))
	for _, id := range names {
		fmt.Fprintf(&b, "  %s: %d client(s)\n", id, s.hubs.get(id).clientCount())
	}
	w.Header().Set("Content-Type", "text/plain; charset=utf-8")
	_, _ = w.Write([]byte(b.String()))
}

// lockedWriter keeps the relay's own lines from interleaving with net/http's.
type lockedWriter struct{}

func newLockedWriter() *lockedWriter { return &lockedWriter{} }

func (*lockedWriter) Write(p []byte) (int, error) {
	logMu.Lock()
	defer logMu.Unlock()
	return log.Writer().Write(p)
}

const version = "0.1.0"
