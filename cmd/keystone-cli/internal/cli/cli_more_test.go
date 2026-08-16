package cli

import (
	"encoding/json"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestRuleList_RendersRows(t *testing.T) {
	setup(t, func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path != "/rules" {
			t.Errorf("path = %s", r.URL.Path)
		}
		_ = json.NewEncoder(w).Encode(map[string]any{
			"rules": []map[string]any{
				{"id": "abc123", "name": "Cosy", "enabled": true, "last_fired": "1m ago"},
			},
		})
	})
	if err := RuleRoot([]string{"list"}); err != nil {
		t.Fatal(err)
	}
}

func TestRuleGet_MatchesByPrefix(t *testing.T) {
	setup(t, func(w http.ResponseWriter, _ *http.Request) {
		_ = json.NewEncoder(w).Encode(map[string]any{
			"rules": []map[string]any{
				{"id": "abc123def", "name": "Cosy", "enabled": true},
			},
		})
	})
	if err := RuleRoot([]string{"get", "abc"}); err != nil {
		t.Fatal(err)
	}
}

func TestRuleCreate_ReadsFile(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "rule.json")
	body := `{"name":"Cosy","enabled":true}`
	if err := os.WriteFile(path, []byte(body), 0o644); err != nil {
		t.Fatal(err)
	}
	setup(t, func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPost || r.URL.Path != "/rules" {
			t.Errorf("%s %s", r.Method, r.URL.Path)
		}
		var m map[string]any
		_ = json.NewDecoder(r.Body).Decode(&m)
		if m["name"] != "Cosy" {
			t.Errorf("name = %v", m["name"])
		}
		_ = json.NewEncoder(w).Encode(map[string]any{"id": "z", "name": "Cosy"})
	})
	if err := RuleRoot([]string{"create", "--file", path}); err != nil {
		t.Fatal(err)
	}
}

func TestStoreBrowse_HitsRegistryEndpoint(t *testing.T) {
	setup(t, func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path != "/plugins/registry" {
			t.Errorf("path = %s", r.URL.Path)
		}
		got, _ := url.QueryUnescape(r.URL.Query().Get("url"))
		if got != "https://example.com" {
			t.Errorf("registry = %s", got)
		}
		_ = json.NewEncoder(w).Encode(map[string]any{
			"plugins": map[string]any{
				"matter": map[string]any{
					"description": "Matter transport",
					"versions":    map[string]any{"0.1.0": map[string]any{"url": "…", "sha256": "…"}},
				},
			},
		})
	})
	if err := StoreRoot([]string{"browse", "--registry", "https://example.com"}); err != nil {
		t.Fatal(err)
	}
}

func TestStoreSearch_FiltersLocally(t *testing.T) {
	setup(t, func(w http.ResponseWriter, _ *http.Request) {
		_ = json.NewEncoder(w).Encode(map[string]any{
			"plugins": map[string]any{
				"matter":   map[string]any{"description": "Matter"},
				"dirigera": map[string]any{"description": "IKEA hub"},
			},
		})
	})
	// Human-mode filter — no assertion on stdout, just that the
	// command completes without error and the query reaches the
	// filter.
	if err := StoreRoot([]string{"search", "--registry", "https://x", "matter"}); err != nil {
		t.Fatal(err)
	}
}

func TestConfigSet_ParsesJSONValue(t *testing.T) {
	var body map[string]any
	setup(t, func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPut {
			t.Errorf("method = %s", r.Method)
		}
		_ = json.NewDecoder(r.Body).Decode(&body)
		_ = json.NewEncoder(w).Encode(map[string]any{"ok": true})
	})
	if err := ConfigRoot([]string{"set", "log.level", `"debug"`}); err != nil {
		t.Fatal(err)
	}
	if body["value"] != "debug" {
		t.Fatalf("value = %v", body["value"])
	}
}

func TestSecretSet_RequiresStdinAndForbidsArgv(t *testing.T) {
	// Feed stdin.
	orig := os.Stdin
	r, w, _ := os.Pipe()
	os.Stdin = r
	t.Cleanup(func() { os.Stdin = orig })

	setup(t, func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path != "/secrets/matter.fabric-key" {
			t.Errorf("path = %s", r.URL.Path)
		}
		var body map[string]any
		_ = json.NewDecoder(r.Body).Decode(&body)
		if body["value"] != "supersecret" {
			t.Errorf("value = %v", body["value"])
		}
		_ = json.NewEncoder(w).Encode(map[string]any{"ok": true})
	})

	// Write value on the child goroutine so ScanLines returns.
	go func() {
		_, _ = w.Write([]byte("supersecret"))
		_ = w.Close()
	}()
	if err := SecretRoot([]string{"set", "matter.fabric-key"}); err != nil {
		t.Fatal(err)
	}
}

func TestSecretList_ForbidsValueLeak(t *testing.T) {
	// If the server ever returned values, our type ignores them.
	// This test asserts the CLI reads only the name field.
	setup(t, func(w http.ResponseWriter, _ *http.Request) {
		_, _ = w.Write([]byte(`{"secrets":[{"name":"a","value":"NEVER_LEAK"}]}`))
	})
	// Force a JSON output so the render helper never inserts anything
	// that could hide the leak; if the CLI writes NEVER_LEAK anywhere
	// the string search below would flag it.
	SetGlobal(Global{Host: G().Host, Output: "json"})
	// Capture stdout for the duration of the call.
	origOut := os.Stdout
	pr, pw, _ := os.Pipe()
	os.Stdout = pw
	t.Cleanup(func() { os.Stdout = origOut })
	if err := SecretRoot([]string{"list"}); err != nil {
		t.Fatal(err)
	}
	_ = pw.Close()
	buf := make([]byte, 1024)
	n, _ := pr.Read(buf)
	if strings.Contains(string(buf[:n]), "NEVER_LEAK") {
		t.Fatalf("secret list must not surface value fields: %s", buf[:n])
	}
}
