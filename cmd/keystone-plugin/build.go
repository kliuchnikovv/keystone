package main

import (
	"flag"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"

	"github.com/kliuchnikovv/keystone/internal/plugin"
)

func runBuild(args []string) error {
	fs := flag.NewFlagSet("build", flag.ContinueOnError)
	dir := fs.String("dir", ".", "plugin directory (must contain plugin.yaml)")
	out := fs.String("o", "", "output path (defaults to bin/<manifest.name>-plugin)")
	tag := fs.String("tag", "", "-tags value forwarded to go build")
	if err := fs.Parse(args); err != nil {
		return err
	}
	mf, err := plugin.ParseFile(filepath.Join(*dir, plugin.ManifestFileName))
	if err != nil {
		return fmt.Errorf("build: %w", err)
	}

	binPath := *out
	if binPath == "" {
		binPath = filepath.Join(*dir, "bin", mf.Metadata.Name+"-plugin")
	}
	if err := os.MkdirAll(filepath.Dir(binPath), 0o755); err != nil {
		return err
	}

	cmdArgs := []string{"build", "-o", binPath}
	if *tag != "" {
		cmdArgs = append(cmdArgs, "-tags", *tag)
	}
	// Build the package that is the plugin dir itself — the SDK conventions
	// put main.go at the plugin root, so "." from that dir is the target.
	cmdArgs = append(cmdArgs, ".")

	cmd := exec.Command("go", cmdArgs...)
	cmd.Dir = *dir
	cmd.Stdout = os.Stdout
	cmd.Stderr = os.Stderr
	if err := cmd.Run(); err != nil {
		return fmt.Errorf("go build failed: %w", err)
	}
	fmt.Println("built", binPath)
	return nil
}
