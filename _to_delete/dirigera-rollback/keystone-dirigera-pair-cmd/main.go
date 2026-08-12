// keystone-dirigera-pair is a small interactive CLI that pairs the local
// DIRIGERA hub with keystone. It handles the OAuth-like flow (challenge +
// physical button press + code exchange) and writes the result to
// <data>/dirigera.json.
//
// Usage:
//
//	keystone-dirigera-pair -host 192.168.0.42 -data ./keystone-data
//
// You'll be prompted to press the physical action button on the DIRIGERA hub
// within ~60 seconds. Once paired, the token is stored in dirigera.json and
// keystone (main binary) will start the adapter automatically on next launch.
package main

import (
	"bufio"
	"context"
	"flag"
	"fmt"
	"os"
	"time"

	"github.com/kliuchnikovv/keystone/internal/adapters/dirigera"
)

func main() {
	host := flag.String("host", "", "DIRIGERA hub IP or hostname (mandatory)")
	dataDir := flag.String("data", "./keystone-data", "keystone data directory (where dirigera.json is written)")
	timeout := flag.Duration("timeout", 90*time.Second, "how long to wait for the button press")
	flag.Parse()

	if *host == "" {
		fmt.Fprintln(os.Stderr, "error: -host required (e.g. 192.168.0.42 or dirigera.local)")
		flag.Usage()
		os.Exit(2)
	}

	ctx, cancel := context.WithTimeout(context.Background(), *timeout)
	defer cancel()

	client := dirigera.NewPairingClient(*host)

	verifier, err := dirigera.GenerateCodeVerifier()
	must("generate verifier", err)
	challenge := dirigera.CodeChallenge(verifier)

	fmt.Println("→ requesting authorization code from", *host)
	code, err := client.RequestCode(ctx, challenge)
	must("request code", err)
	fmt.Println("✓ got code:", short(code))

	fmt.Println()
	fmt.Println("╔══════════════════════════════════════════════════════════════╗")
	fmt.Println("║                                                              ║")
	fmt.Println("║   PRESS THE PHYSICAL ACTION BUTTON ON THE DIRIGERA HUB       ║")
	fmt.Println("║   (small button on the bottom of the hub)                    ║")
	fmt.Println("║                                                              ║")
	fmt.Println("║   You have up to 60 seconds. Press Enter to try immediately, ║")
	fmt.Println("║   or wait — we'll retry every 3 seconds.                     ║")
	fmt.Println("║                                                              ║")
	fmt.Println("╚══════════════════════════════════════════════════════════════╝")
	fmt.Println()

	// Kick a reader for early Enter.
	go func() {
		bufio.NewReader(os.Stdin).ReadBytes('\n')
	}()

	var token string
	deadline := time.Now().Add(*timeout)
	attempt := 0
	for time.Now().Before(deadline) {
		attempt++
		token, err = client.ExchangeCode(ctx, code, verifier)
		if err == nil {
			break
		}
		fmt.Printf("  … attempt %d: %s\n", attempt, err)
		select {
		case <-time.After(3 * time.Second):
		case <-ctx.Done():
			must("exchange", ctx.Err())
		}
	}
	if token == "" {
		must("exchange", fmt.Errorf("timed out waiting for button press"))
	}

	fmt.Println()
	fmt.Println("✓ got access token:", short(token))

	cfg := &dirigera.Config{Host: *host, Token: token}
	must("save config", dirigera.SaveConfig(*dataDir, cfg))
	fmt.Println("✓ written to", *dataDir+"/dirigera.json")
	fmt.Println()
	fmt.Println("Now (re)start keystone — the DIRIGERA adapter will pick this up automatically.")
}

func must(label string, err error) {
	if err == nil {
		return
	}
	fmt.Fprintln(os.Stderr, "✗", label, ":", err)
	os.Exit(1)
}

func short(s string) string {
	if len(s) <= 12 {
		return s
	}
	return s[:6] + "…" + s[len(s)-4:]
}
