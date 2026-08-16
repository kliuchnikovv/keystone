package manager

import (
	"strings"
	"testing"

	"github.com/kliuchnikovv/keystone/internal/plugin"
)

func TestSidecarCommand_ByRuntime(t *testing.T) {
	cases := []struct {
		runtime string
		exec    string
		want    []string
	}{
		{"static", "bin/native", []string{"bin/native"}},
		{"", "bin/native", []string{"bin/native"}},
		{"node20", "dist/index.js", []string{"node", "dist/index.js"}},
		{"node22", "dist/index.mjs", []string{"node", "dist/index.mjs"}},
		{"python3.11", "app.py", []string{"python3", "app.py"}},
	}
	for _, tc := range cases {
		t.Run(tc.runtime, func(t *testing.T) {
			sc := plugin.Sidecar{Runtime: plugin.Runtime(tc.runtime), Exec: tc.exec}
			got, err := sidecarCommand(sc, "/plug")
			if err != nil {
				t.Fatal(err)
			}
			// exec becomes absolute (joined with pluginDir) — check suffix.
			if !strings.HasSuffix(got[len(got)-1], tc.exec) {
				t.Fatalf("last arg = %q, want suffix %q", got[len(got)-1], tc.exec)
			}
			// Interpreter prefix matches expectation.
			if len(tc.want) > 1 && got[0] != tc.want[0] {
				t.Fatalf("interpreter = %q, want %q", got[0], tc.want[0])
			}
		})
	}
}

func TestSidecarEnv_ExpandsBaseReferences(t *testing.T) {
	base := map[string]string{
		"KEYSTONE_PLUGIN_DATA": "/data/matter",
		"KEYSTONE_PLUGIN_NAME": "matter",
	}
	sc := map[string]string{
		"MATTER_STORAGE": "$KEYSTONE_PLUGIN_DATA/fabric",
	}
	got := sidecarEnv(sc, base)

	found := false
	for _, kv := range got {
		if kv == "MATTER_STORAGE=/data/matter/fabric" {
			found = true
		}
	}
	if !found {
		t.Fatalf("expected MATTER_STORAGE=/data/matter/fabric in %v", got)
	}
}
