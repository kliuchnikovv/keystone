package main

import (
	"archive/tar"
	"compress/gzip"
	"flag"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"

	"github.com/kliuchnikovv/keystone/internal/plugin"
)

// packedPaths are the subtrees keystone-plugin packs into the tarball.
// Anything else stays behind — no ".git", no vendored source, no local
// scratch. The manifest itself is always included at the root.
var packedPaths = []string{"bin", "sidecars", "ui"}

func runPublish(args []string) error {
	fs := flag.NewFlagSet("publish", flag.ContinueOnError)
	dir := fs.String("dir", ".", "plugin directory")
	out := fs.String("o", "", "output path for the tarball (defaults to <name>-<version>.tgz)")
	registry := fs.String("registry", "", "registry to push to (v0.1 emits a warning — remote push lands with plugin-registry)")
	if err := fs.Parse(args); err != nil {
		return err
	}

	mf, err := plugin.ParseFile(filepath.Join(*dir, plugin.ManifestFileName))
	if err != nil {
		return fmt.Errorf("publish: %w", err)
	}
	if *out == "" {
		*out = fmt.Sprintf("%s-%s.tgz", mf.Metadata.Name, mf.Metadata.Version)
	}

	if err := writeTarball(*dir, *out); err != nil {
		return err
	}
	fmt.Println("packed", *out)

	if *registry != "" {
		fmt.Fprintln(os.Stderr, "warning: remote registry push is not implemented yet — plugin-registry integration lands in a follow-up. The tarball above is ready to upload manually.")
	}
	return nil
}

func writeTarball(pluginDir, outPath string) error {
	f, err := os.Create(outPath)
	if err != nil {
		return err
	}
	defer f.Close()

	gz := gzip.NewWriter(f)
	defer gz.Close()
	tw := tar.NewWriter(gz)
	defer tw.Close()

	// Always include the manifest at the root.
	if err := addFile(tw, pluginDir, plugin.ManifestFileName); err != nil {
		return err
	}
	for _, sub := range packedPaths {
		full := filepath.Join(pluginDir, sub)
		info, err := os.Stat(full)
		if err != nil {
			if os.IsNotExist(err) {
				continue
			}
			return err
		}
		if !info.IsDir() {
			continue
		}
		if err := walkAndAdd(tw, pluginDir, sub); err != nil {
			return err
		}
	}
	return nil
}

// walkAndAdd descends sub (relative to pluginDir) and adds every file
// with tar headers whose Name is also relative — the tarball is
// unpacked back into <plugins-dir>/<slug>/ where those paths are
// exactly what the manifest expects.
func walkAndAdd(tw *tar.Writer, pluginDir, sub string) error {
	base := filepath.Join(pluginDir, sub)
	return filepath.Walk(base, func(path string, info os.FileInfo, err error) error {
		if err != nil {
			return err
		}
		rel, err := filepath.Rel(pluginDir, path)
		if err != nil {
			return err
		}
		return addPath(tw, path, rel, info)
	})
}

func addFile(tw *tar.Writer, pluginDir, name string) error {
	full := filepath.Join(pluginDir, name)
	info, err := os.Stat(full)
	if err != nil {
		return err
	}
	return addPath(tw, full, name, info)
}

func addPath(tw *tar.Writer, full, name string, info os.FileInfo) error {
	name = filepath.ToSlash(name)
	if info.IsDir() {
		return tw.WriteHeader(&tar.Header{Name: name + "/", Mode: 0o755, Typeflag: tar.TypeDir})
	}
	hdr := &tar.Header{
		Name:     name,
		Mode:     0o644,
		Size:     info.Size(),
		Typeflag: tar.TypeReg,
	}
	// Preserve the executable bit — the plugin binary needs it or the
	// core cannot exec the entrypoint after unpack.
	if info.Mode()&0o111 != 0 {
		hdr.Mode = 0o755
	}
	if err := tw.WriteHeader(hdr); err != nil {
		return err
	}
	// A tarball dropped into any dir must not carry absolute paths.
	// filepath.Rel already ensured that, but a leading "./" or ".."
	// component would still be surprising; reject those defensively.
	if strings.HasPrefix(name, "..") {
		return fmt.Errorf("refusing to pack path that escapes plugin dir: %s", name)
	}
	f, err := os.Open(full)
	if err != nil {
		return err
	}
	defer f.Close()
	_, err = io.Copy(tw, f)
	return err
}
