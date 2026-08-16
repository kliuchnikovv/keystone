package plugin

import (
	"bytes"
	"flag"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

var update = flag.Bool("update", false, "rewrite the .golden files in testdata/invalid")

// TestValidManifests keeps every example in testdata/valid green. New files are
// picked up automatically — dropping one in is enough to cover a new shape.
func TestValidManifests(t *testing.T) {
	for _, path := range manifests(t, "valid") {
		t.Run(name(path), func(t *testing.T) {
			data, err := os.ReadFile(path)
			if err != nil {
				t.Fatal(err)
			}
			m, err := Parse(bytes.NewReader(data))
			if err != nil {
				t.Fatalf("expected a valid manifest, got:\n%v", err)
			}
			if m.APIVersion != APIVersion || m.Kind != Kind {
				t.Errorf("apiVersion/kind = %q/%q, want %q/%q", m.APIVersion, m.Kind, APIVersion, Kind)
			}
			if m.Metadata.Name != name(path) {
				t.Errorf("metadata.name = %q, want %q (file name and slug should match)", m.Metadata.Name, name(path))
			}
			if len(m.Spec.Capabilities) == 0 {
				t.Error("spec.capabilities decoded empty")
			}
		})
	}
}

// TestInvalidManifests pins the exact report for each broken manifest,
// including line and column — that is the acceptance criterion of the ticket,
// and a golden file is the only way to notice it silently regressing.
func TestInvalidManifests(t *testing.T) {
	for _, path := range manifests(t, "invalid") {
		t.Run(name(path), func(t *testing.T) {
			data, err := os.ReadFile(path)
			if err != nil {
				t.Fatal(err)
			}
			err = Validate(bytes.NewReader(data))
			if err == nil {
				t.Fatal("expected the manifest to be rejected, got nil")
			}
			var verr *ValidationErrors
			if !asValidationErrors(err, &verr) {
				t.Fatalf("expected *ValidationErrors, got %T: %v", err, err)
			}
			for _, e := range verr.Errors {
				if e.Line <= 0 {
					t.Errorf("error %q carries no line number", e.String())
				}
			}

			golden := strings.TrimSuffix(path, ".yaml") + ".golden"
			got := err.Error() + "\n"
			if *update {
				if err := os.WriteFile(golden, []byte(got), 0o644); err != nil {
					t.Fatal(err)
				}
				return
			}
			want, err := os.ReadFile(golden)
			if err != nil {
				t.Fatalf("%v (run: go test ./internal/plugin/ -update)", err)
			}
			if got != string(want) {
				t.Errorf("report mismatch\n--- got ---\n%s\n--- want ---\n%s", got, want)
			}
		})
	}
}

func TestValidateRejectsBrokenYAML(t *testing.T) {
	broken := "apiVersion: keystone.plugin/v1\nmetadata:\n  name: x\n   displayName: y\n"
	err := Validate(strings.NewReader(broken))
	if err == nil {
		t.Fatal("expected a syntax error, got nil")
	}
	var verr *ValidationErrors
	if asValidationErrors(err, &verr) {
		t.Fatalf("a yaml syntax error should not be reported as schema violations: %v", err)
	}
	if !strings.Contains(err.Error(), "line 4") {
		t.Errorf("syntax error should name the line, got: %v", err)
	}
}

func TestValidateRejectsEmptyInput(t *testing.T) {
	if err := Validate(strings.NewReader("")); err == nil {
		t.Fatal("expected an empty manifest to be rejected")
	}
}

func TestParseFileAcceptsPluginDirectory(t *testing.T) {
	dir := t.TempDir()
	src, err := os.ReadFile(filepath.Join("testdata", "valid", "dirigera.yaml"))
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, ManifestFileName), src, 0o644); err != nil {
		t.Fatal(err)
	}
	m, err := ParseFile(dir)
	if err != nil {
		t.Fatalf("ParseFile(dir): %v", err)
	}
	if m.Metadata.Name != "dirigera" {
		t.Errorf("metadata.name = %q, want dirigera", m.Metadata.Name)
	}
}

// TestValidateFilePrefixesSource keeps the CLI-facing format (path:line:col)
// distinct from the reader-facing one (line:col).
func TestValidateFilePrefixesSource(t *testing.T) {
	path := filepath.Join("testdata", "invalid", "bad-restart.yaml")
	err := ValidateFile(path)
	if err == nil {
		t.Fatal("expected the manifest to be rejected")
	}
	if !strings.HasPrefix(err.Error(), path+":") {
		t.Errorf("message should start with the file path, got: %v", err)
	}
}

func TestPermissionSpellings(t *testing.T) {
	m, err := ParseFile(filepath.Join("testdata", "valid", "matter.yaml"))
	if err != nil {
		t.Fatal(err)
	}
	want := map[string][]string{
		"network.lan":        nil,
		"secretstore.read":   {"matter.fabric-key"},
		"storage.persistent": nil,
		"network.multicast":  nil,
		"secretstore.write":  {"matter.fabric-key"},
	}
	if len(m.Spec.Permissions) != len(want) {
		t.Fatalf("decoded %d permissions, want %d", len(m.Spec.Permissions), len(want))
	}
	for _, p := range m.Spec.Permissions {
		scopes, known := want[p.Name]
		if !known {
			t.Errorf("unexpected permission %q", p.Name)
			continue
		}
		if strings.Join(p.Scopes, ",") != strings.Join(scopes, ",") {
			t.Errorf("permission %q scopes = %v, want %v", p.Name, p.Scopes, scopes)
		}
	}
}

func TestRestartPolicyDefaults(t *testing.T) {
	if got := (Sidecar{}).RestartPolicy(); got != RestartOnFailure {
		t.Errorf("default restart policy = %q, want %q", got, RestartOnFailure)
	}
	if got := (Sidecar{Restart: RestartNever}).RestartPolicy(); got != RestartNever {
		t.Errorf("explicit restart policy = %q, want %q", got, RestartNever)
	}
}

func TestSchemaJSONIsACopy(t *testing.T) {
	first := SchemaJSON()
	first[0] = 'X'
	if SchemaJSON()[0] == 'X' {
		t.Error("SchemaJSON handed out the embedded buffer itself")
	}
}

func manifests(t *testing.T, kind string) []string {
	t.Helper()
	paths, err := filepath.Glob(filepath.Join("testdata", kind, "*.yaml"))
	if err != nil {
		t.Fatal(err)
	}
	if len(paths) < 5 {
		t.Fatalf("testdata/%s holds %d manifests, want at least 5", kind, len(paths))
	}
	return paths
}

func name(path string) string {
	return strings.TrimSuffix(filepath.Base(path), ".yaml")
}

func asValidationErrors(err error, target **ValidationErrors) bool {
	v, ok := err.(*ValidationErrors)
	if ok {
		*target = v
	}
	return ok
}
