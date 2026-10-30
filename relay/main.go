// Keystone Relay — remote access for the hub.
//
// A hub (or a tunnel agent on the hub's LAN) dials out and registers a tunnel;
// a remote client connects and is wired through to that hub.
//
//	relay --port 9090 --secret <shared-secret>
//
// Environment:
//
//	PORT                    the port to listen on (a PaaS sets it)
//	RELAY_SECRET            the shared secret the hubs authenticate with — required
//	                        unless every hub has a secret of its own
//	RELAY_MASTER_SECRET     the key every hub's secret is derived from: a new home
//	                        needs nothing here, the hub carries its secret in its
//	                        firmware and the relay derives the same value
//	RELAY_HUB_SECRETS       "id:secret,id2:secret2" — a secret per hub, for the
//	                        hubs that have one of their own
//	RELAY_STATUS_TOKEN      if set, the status page shows the hub ids only to
//	                        whoever sends this token; without it, counters only
//	RELAY_ALLOWED_ORIGINS   comma-separated browser origins allowed to connect
//	                        (the relay's own origin is always allowed)
//
// TLS is terminated in front of this process: the hub verifies the relay's
// certificate against the Mozilla CA bundle, so the certificate has to come
// from a public CA.
//
// See docs/relay-security.md for the threat model.
package main

import (
	"flag"
	"fmt"
	"net/http"
	"os"
	"os/signal"
	"strconv"
	"strings"
	"syscall"
	"time"
)

func main() {
	port := flag.Int("port", 0, "port to listen on (0: the PORT variable, or 9090)")
	secret := flag.String("secret", "", "shared secret the hubs authenticate with (default: the RELAY_SECRET variable)")
	maxClients := flag.Int("max-clients", MaxClientsPerHub, "remote clients per hub")
	maxHubs := flag.Int("max-hubs", MaxHubs, "hubs this relay serves")
	flag.Parse()

	listenPort := *port
	if listenPort == 0 {
		listenPort = 9090
		if p := os.Getenv("PORT"); p != "" {
			if n, err := strconv.Atoi(p); err == nil {
				listenPort = n
			}
		}
	}

	sec := *secret
	if sec == "" {
		sec = os.Getenv("RELAY_SECRET")
	}
	// The master key: the relay derives the secret of every hub from it, so a new
	// home needs nothing here - the hub carries its secret in its firmware.
	master := os.Getenv("RELAY_MASTER_SECRET")
	// Some hubs have a secret of their own, given as a pair.
	hubSecrets := ParseHubSecrets(os.Getenv("RELAY_HUB_SECRETS"))
	if sec == "" && master == "" && len(hubSecrets) == 0 {
		fmt.Fprintln(os.Stderr,
			"relay: no secret at all. Set RELAY_MASTER_SECRET (the relay derives every "+
				"hub's secret from it), RELAY_SECRET (one secret for every hub) or "+
				"RELAY_HUB_SECRETS (\"id:secret,...\", one pair per hub) - without one, "+
				"anybody could register a tunnel under your hub id.")
		os.Exit(2)
	}
	if len(sec) < 16 && sec != "" {
		fmt.Fprintln(os.Stderr,
			"relay: the shared secret is short. Use at least 16 random characters — it is "+
				"the only thing that keeps a stranger from registering a tunnel under your hub id.")
	}

	statusToken := os.Getenv("RELAY_STATUS_TOKEN")
	origins := ParseOriginAllowlist(os.Getenv("RELAY_ALLOWED_ORIGINS"))
	r := newRelay(sec, statusToken, origins, *maxClients, *maxHubs)
	r.setMaster(master)
	r.setHubSecrets(hubSecrets)

	srv := &http.Server{
		Addr:              fmt.Sprintf("0.0.0.0:%d", listenPort),
		Handler:           r,
		ReadHeaderTimeout: 10 * time.Second,
	}

	// Forget the counters of sources that have not failed for a while.
	go func() {
		t := time.NewTicker(time.Minute)
		defer t.Stop()
		for range t.C {
			r.limiter.Sweep(time.Now())
		}
	}()

	stop := make(chan os.Signal, 1)
	signal.Notify(stop, os.Interrupt, syscall.SIGTERM)
	go func() {
		<-stop
		r.log("relay", "shutting down")
		_ = srv.Close()
	}()

	r.log("relay", fmt.Sprintf("listening on port %d", listenPort))
	if master != "" {
		r.log("relay", "master key: set (every hub's secret is derived from it)")
	}
	if len(hubSecrets) > 0 {
		r.log("relay", fmt.Sprintf("%d hub(s) with a secret of their own", len(hubSecrets)))
	}
	if statusToken != "" {
		r.log("relay", "status page: token required")
	} else {
		r.log("relay", "status page: counters only")
	}
	if len(origins) > 0 {
		r.log("relay", "browser origins allowed: "+strings.Join(origins, ", "))
	} else {
		r.log("relay", "browser origins allowed: none (the relay's own origin is always allowed)")
	}

	if err := srv.ListenAndServe(); err != nil && err != http.ErrServerClosed {
		fmt.Fprintln(os.Stderr, "relay:", err)
		os.Exit(1)
	}
}
