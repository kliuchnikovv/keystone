package dirigera

import (
	"bytes"
	"context"
	"crypto/tls"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"time"
)

// Client is a thin HTTPS client to a DIRIGERA hub. Uses bearer-token auth
// and skips TLS verification (self-signed cert, LAN-only).
type Client struct {
	host   string        // hostname or IP; port 8443 is appended if missing
	token  string        // bearer token from pairing
	http   *http.Client
	name   string        // pairing name (shown in DIRIGERA app under Integrations)
}

// NewClient builds a client. httpTimeout is per-request; 10s is sane.
func NewClient(host, token string, httpTimeout time.Duration) *Client {
	tr := &http.Transport{
		TLSClientConfig: &tls.Config{InsecureSkipVerify: true},
		MaxIdleConns:    4,
		IdleConnTimeout: 60 * time.Second,
	}
	return &Client{
		host:  host,
		token: token,
		http:  &http.Client{Transport: tr, Timeout: httpTimeout},
		name:  "keystone",
	}
}

func (c *Client) baseURL() string {
	// Accept both "1.2.3.4" and "1.2.3.4:8443".
	if hasPort(c.host) {
		return "https://" + c.host
	}
	return "https://" + c.host + ":8443"
}

func hasPort(s string) bool {
	for i := len(s) - 1; i >= 0; i-- {
		if s[i] == ':' {
			return true
		}
		if s[i] == '/' {
			return false
		}
	}
	return false
}

func (c *Client) do(ctx context.Context, method, path string, body any) (*http.Response, error) {
	var buf io.Reader
	if body != nil {
		b, err := json.Marshal(body)
		if err != nil {
			return nil, fmt.Errorf("marshal: %w", err)
		}
		buf = bytes.NewReader(b)
	}
	req, err := http.NewRequestWithContext(ctx, method, c.baseURL()+path, buf)
	if err != nil {
		return nil, err
	}
	if body != nil {
		req.Header.Set("content-type", "application/json")
	}
	if c.token != "" {
		req.Header.Set("Authorization", "Bearer "+c.token)
	}
	res, err := c.http.Do(req)
	if err != nil {
		return nil, err
	}
	if res.StatusCode >= 400 {
		defer res.Body.Close()
		b, _ := io.ReadAll(res.Body)
		return nil, fmt.Errorf("dirigera %s %s: %d %s", method, path, res.StatusCode, string(b))
	}
	return res, nil
}

// ListDevices returns all devices from the hub.
func (c *Client) ListDevices(ctx context.Context) ([]Device, error) {
	res, err := c.do(ctx, "GET", "/v1/devices", nil)
	if err != nil {
		return nil, err
	}
	defer res.Body.Close()
	var out []Device
	if err := json.NewDecoder(res.Body).Decode(&out); err != nil {
		return nil, fmt.Errorf("decode: %w", err)
	}
	return out, nil
}

// GetDevice fetches one device by id.
func (c *Client) GetDevice(ctx context.Context, id string) (*Device, error) {
	res, err := c.do(ctx, "GET", "/v1/devices/"+id, nil)
	if err != nil {
		return nil, err
	}
	defer res.Body.Close()
	var out Device
	if err := json.NewDecoder(res.Body).Decode(&out); err != nil {
		return nil, err
	}
	return &out, nil
}

// PatchDeviceAttributes sends a PATCH with the given attributes map.
//
// Example: PatchDeviceAttributes(ctx, id, map[string]any{"isOn": true, "lightLevel": 50})
func (c *Client) PatchDeviceAttributes(ctx context.Context, id string, attrs map[string]any) error {
	body := []PatchAttribute{{Attributes: attrs}}
	res, err := c.do(ctx, "PATCH", "/v1/devices/"+id, body)
	if err != nil {
		return err
	}
	res.Body.Close()
	return nil
}

// HubStatus is a small ping to check host + token liveness.
func (c *Client) HubStatus(ctx context.Context) error {
	res, err := c.do(ctx, "GET", "/v1/hub/status", nil)
	if err != nil {
		return err
	}
	res.Body.Close()
	return nil
}

// ---- Pairing (unauthenticated) ----

// PairingClient is a pre-token client used only for the OAuth-like flow.
func NewPairingClient(host string) *Client {
	return NewClient(host, "", 65*time.Second)
}

// RequestCode initiates pairing: POST /v1/oauth/authorize.
// Returns the code that must be presented to /v1/oauth/token after the user
// physically presses the DIRIGERA hub's button.
func (c *Client) RequestCode(ctx context.Context, codeChallenge string) (string, error) {
	u := fmt.Sprintf("/v1/oauth/authorize?audience=homesmart.local&response_type=code&code_challenge_method=S256&code_challenge=%s", codeChallenge)
	res, err := c.do(ctx, "POST", u, nil)
	if err != nil {
		return "", err
	}
	defer res.Body.Close()
	var out AuthorizeResponse
	if err := json.NewDecoder(res.Body).Decode(&out); err != nil {
		return "", err
	}
	return out.Code, nil
}

// ExchangeCode completes pairing: POST /v1/oauth/token with the code + verifier.
// This will only succeed if the user has pressed the physical action button on
// the DIRIGERA hub within ~60 seconds of the RequestCode call.
func (c *Client) ExchangeCode(ctx context.Context, code, codeVerifier string) (string, error) {
	// The token endpoint expects form-encoded body, not JSON.
	form := fmt.Sprintf("code=%s&name=%s&grant_type=authorization_code&code_verifier=%s",
		code, c.name, codeVerifier)
	req, err := http.NewRequestWithContext(ctx, "POST", c.baseURL()+"/v1/oauth/token",
		bytes.NewReader([]byte(form)))
	if err != nil {
		return "", err
	}
	req.Header.Set("content-type", "application/x-www-form-urlencoded")
	res, err := c.http.Do(req)
	if err != nil {
		return "", err
	}
	defer res.Body.Close()
	if res.StatusCode >= 400 {
		b, _ := io.ReadAll(res.Body)
		return "", fmt.Errorf("token: %d %s", res.StatusCode, string(b))
	}
	var out TokenResponse
	if err := json.NewDecoder(res.Body).Decode(&out); err != nil {
		return "", err
	}
	return out.AccessToken, nil
}
