package main

import (
	"flag"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"

	"github.com/kliuchnikovv/keystone/internal/plugin"
)

func runTest(args []string) error {
	fs := flag.NewFlagSet("test", flag.ContinueOnError)
	dir := fs.String("dir", ".", "plugin directory")
	skipGoTest := fs.Bool("skip-go-test", false, "only validate the manifest; do not run go test")
	if err := fs.Parse(args); err != nil {
		return err
	}
	// Manifest validation is always cheap and catches the most common
	// mistake (misspelled fields, bad JSON schema, non-relative exec).
	// We run it first so a broken manifest is not hidden by an
	// unrelated go test failure.
	if _, err := plugin.ParseFile(filepath.Join(*dir, plugin.ManifestFileName)); err != nil {
		return fmt.Errorf("test: manifest: %w", err)
	}
	fmt.Println("manifest ok")
	if *skipGoTest {
		return nil
	}
	cmd := exec.Command("go", "test", "./...")
	cmd.Dir = *dir
	cmd.Stdout = os.Stdout
	cmd.Stderr = os.Stderr
	if err := cmd.Run(); err != nil {
		return fmt.Errorf("go test failed: %w", err)
	}
	return nil
}
