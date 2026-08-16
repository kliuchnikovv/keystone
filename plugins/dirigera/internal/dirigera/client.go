// Package dirigera talks HTTP+bearer to an IKEA DIRIGERA hub.
//
// The hub speaks REST on https://<host>:8443 with a self-signed cert
// and a per-app bearer token you get once via a physical pairing flow
// (button press on the hub). Everything below expects an already-paired
// token; the pairing helper lives in Commission.
package dirigera

import (
	"bytes"
	"context"
	"crypto/sha256"
	"crypto/tls"
	"crypto/x509"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"strings"
	"time"
)

// Client is a lean HTTP client for the DIRIGERA v1 API. Concurrent use
// is safe as long as callers don't reconfigure Token or BaseURL from
// under an in-flight request.
type Client struct {
	BaseURL string
	Token   string
	HTTP    *http.Client
}

// Config carries what NewClient needs to construct a trusted transport.
// The hub ships a self-signed certificate, so at least one of CAPEM or
// PinnedSHA256 must be set — DIRIGERA has no CA an OS trust store
// recognises, and skipping verification is not an option.
type Config struct {
	// BaseURL is the hub root, e.g. https://192.168.1.10:8443.
	BaseURL string
	// Token is the per-app bearer minted during Commission.
	Token string
	// CAPEM is a PEM-encoded certificate the plugin should trust as a
	// CA when validating the hub. When set, standard TLS verification
	// runs against exactly this cert.
	CAPEM []byte
	// PinnedSHA256 is the hex-encoded SHA-256 fingerprint of the hub's
	// leaf certificate. Colons are tolerated ("AA:BB:..."). When set,
	// verification uses fingerprint pinning instead of a CA chain.
	PinnedSHA256 string
}

// NewClient builds a Client with a properly-verified TLS transport.
// Returns an error if neither CAPEM nor PinnedSHA256 is set — a plugin
// author must decide how to trust the hub, and skipping verification
// is not a supported mode.
func NewClient(cfg Config) (*Client, error) {
	tlsCfg, err := buildTLSConfig(cfg)
	if err != nil {
		return nil, err
	}
	return &Client{
		BaseURL: cfg.BaseURL,
		Token:   cfg.Token,
		HTTP: &http.Client{
			Timeout:   15 * time.Second,
			Transport: &http.Transport{TLSClientConfig: tlsCfg},
		},
	}, nil
}

func buildTLSConfig(cfg Config) (*tls.Config, error) {
	switch {
	case len(cfg.CAPEM) > 0:
		pool := x509.NewCertPool()
		if !pool.AppendCertsFromPEM(cfg.CAPEM) {
			return nil, errors.New("dirigera: CAPEM does not contain a valid PEM certificate")
		}
		return &tls.Config{RootCAs: pool, MinVersion: tls.VersionTLS12}, nil
	case cfg.PinnedSHA256 != "":
		want, err := hex.DecodeString(strings.NewReplacer(":", "", " ", "").Replace(cfg.PinnedSHA256))
		if err != nil || len(want) != sha256.Size {
			return nil, fmt.Errorf("dirigera: PinnedSHA256 must be a hex SHA-256 (64 chars), got %q", cfg.PinnedSHA256)
		}
		// VerifyPeerCertificate runs *after* the stdlib chain check,
		// which we still want off (self-signed will never chain), so
		// disable the default chain and do our own leaf comparison.
		return &tls.Config{
			InsecureSkipVerify: true, //nolint:gosec // Pinning below is our verification path.
			MinVersion:         tls.VersionTLS12,
			VerifyPeerCertificate: func(rawCerts [][]byte, _ [][]*x509.Certificate) error {
				if len(rawCerts) == 0 {
					return errors.New("dirigera: hub presented no certificate")
				}
				got := sha256.Sum256(rawCerts[0])
				if !bytesEqual(got[:], want) {
					return fmt.Errorf("dirigera: certificate fingerprint mismatch (got %s)", hex.EncodeToString(got[:]))
				}
				return nil
			},
		}, nil
	default:
		return nil, errors.New("dirigera: Config needs CAPEM or PinnedSHA256 — TLS verification is not optional")
	}
}

func bytesEqual(a, b []byte) bool {
	if len(a) != len(b) {
		return false
	}
	var v byte
	for i := range a {
		v |= a[i] ^ b[i]
	}
	return v == 0
}

// Device is one DIRIGERA device row.
type Device struct {
	ID           string         `json:"id"`
	Type         string         `json:"type"`
	DeviceType   string         `json:"deviceType"`
	CreatedAt    string         `json:"createdAt,omitempty"`
	Attributes   map[string]any `json:"attributes"`
	Capabilities struct {
		CanSend    []string `json:"canSend,omitempty"`
		CanReceive []string `json:"canReceive,omitempty"`
	} `json:"capabilities"`
	Room struct {
		Name string `json:"name,omitempty"`
	} `json:"room,omitempty"`
}

// ListDevices reads /v1/devices.
func (c *Client) ListDevices(ctx context.Context) ([]Device, error) {
	var out []Device
	if err := c.doJSON(ctx, http.MethodGet, "/v1/devices", nil, &out); err != nil {
		return nil, err
	}
	return out, nil
}

// GetDevice reads /v1/devices/{id}.
func (c *Client) GetDevice(ctx context.Context, id string) (*Device, error) {
	var out Device
	if err := c.doJSON(ctx, http.MethodGet, "/v1/devices/"+id, nil, &out); err != nil {
		return nil, err
	}
	return &out, nil
}

// SetAttributes patches one device's attributes. The hub accepts a
// single-object body under the "attributes" key.
func (c *Client) SetAttributes(ctx context.Context, id string, attrs map[string]any) error {
	body := []map[string]any{{"attributes": attrs}}
	return c.doJSON(ctx, http.MethodPatch, "/v1/devices/"+id, body, nil)
}

// RemoveDevice unpairs a device from the hub.
func (c *Client) RemoveDevice(ctx context.Context, id string) error {
	return c.doJSON(ctx, http.MethodDelete, "/v1/devices/"+id, nil, nil)
}

// ErrUnauthorized is returned when the hub rejects the bearer token —
// distinct from a device-level "not found" because the fix is different
// (re-pair, not retry).
var ErrUnauthorized = errors.New("dirigera: hub rejected token")

// ErrNotFound is returned when a device id is unknown to the hub.
var ErrNotFound = errors.New("dirigera: device not found")

func (c *Client) doJSON(ctx context.Context, method, path string, in, out any) error {
	var body io.Reader
	if in != nil {
		raw, err := json.Marshal(in)
		if err != nil {
			return fmt.Errorf("dirigera: encode: %w", err)
		}
		body = bytes.NewReader(raw)
	}
	req, err := http.NewRequestWithContext(ctx, method, c.BaseURL+path, body)
	if err != nil {
		return err
	}
	req.Header.Set("authorization", "Bearer "+c.Token)
	if body != nil {
		req.Header.Set("content-type", "application/json")
	}
	resp, err := c.HTTP.Do(req)
	if err != nil {
		return fmt.Errorf("dirigera: %s %s: %w", method, path, err)
	}
	defer resp.Body.Close()

	switch {
	case resp.StatusCode == http.StatusUnauthorized:
		return ErrUnauthorized
	case resp.StatusCode == http.StatusNotFound:
		return ErrNotFound
	case resp.StatusCode >= 400:
		msg, _ := io.ReadAll(io.LimitReader(resp.Body, 4096))
		return fmt.Errorf("dirigera: %s %s: %s: %s", method, path, resp.Status, msg)
	}
	if out == nil || resp.StatusCode == http.StatusNoContent {
		return nil
	}
	return json.NewDecoder(resp.Body).Decode(out)
}
