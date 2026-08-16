package main

import (
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/kliuchnikovv/keystone/internal/plugin"
)

func TestNew_ScaffoldsValidPlugin(t *testing.T) {
	dir := t.TempDir()
	if err := runNew([]string{
		"--name=demo",
		"--transport=demo",
		"--module=github.com/example/plugin-demo",
		"--dir=" + dir,
	}); err != nil {
		t.Fatalf("runNew: %v", err)
	}
	target := filepath.Join(dir, "demo")
	for _, want := range []string{"plugin.yaml", "main.go", "go.mod", "internal/demo/adapter.go", "README.md"} {
		if _, err := os.Stat(filepath.Join(target, want)); err != nil {
			t.Errorf("scaffold missing %s: %v", want, err)
		}
	}
	if _, err := plugin.ParseFile(filepath.Join(target, plugin.ManifestFileName)); err != nil {
		t.Fatalf("scaffolded manifest fails validation: %v", err)
	}
	main, _ := os.ReadFile(filepath.Join(target, "main.go"))
	if !strings.Contains(string(main), `"github.com/example/plugin-demo/internal/demo"`) {
		t.Errorf("main.go missing internal import: %s", main)
	}
}

func TestNew_RejectsInvalidSlug(t *testing.T) {
	dir := t.TempDir()
	err := runNew([]string{"--name=BadCaps", "--dir=" + dir})
	if err == nil {
		t.Fatal("expected error for uppercase slug")
	}
}

func TestNew_RefusesOverwrite(t *testing.T) {
	dir := t.TempDir()
	if err := runNew([]string{"--name=a", "--dir=" + dir}); err != nil {
		t.Fatal(err)
	}
	if err := runNew([]string{"--name=a", "--dir=" + dir}); err == nil {
		t.Fatal("expected refusal to overwrite existing target")
	}
}
