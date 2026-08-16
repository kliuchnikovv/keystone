// Package pack unpacks and packs plugin tarballs. Shared between the
// keystone-plugin CLI (which packs on publish and unpacks on install)
// and the core's store client (which unpacks after a registry fetch).
//
// The safety guarantees are the same on both sides: refuse tarballs
// with absolute or escaping paths, refuse plugin names that could
// escape the target dir, refuse to overwrite an existing target
// unless the caller explicitly opts in.
package pack

import (
	"archive/tar"
	"bytes"
	"compress/gzip"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"

	"github.com/kliuchnikovv/keystone/internal/plugin"
)

// UnpackTarball extracts a gzipped tarball into <targetRoot>/<name>/,
// where <name> is manifest.metadata.name. Returns that name so the
// caller can reference the installed dir. Both the plugin name and
// each tarball entry are checked for path escapes before any file is
// written.
//
// If <targetRoot>/<name>/ exists, UnpackTarball refuses unless force
// is set. A symlink at the target is always refused — RemoveAll would
// follow it and clobber the wrong tree.
func UnpackTarball(tarballPath, targetRoot string, force bool) (string, error) {
	f, err := os.Open(tarballPath)
	if err != nil {
		return "", err
	}
	defer f.Close()
	return UnpackReader(f, targetRoot, force)
}

// UnpackReader is UnpackTarball for an already-open gzipped tar
// stream — used by the store client, which streams the tarball out
// of an HTTP response.
func UnpackReader(r io.Reader, targetRoot string, force bool) (string, error) {
	gz, err := gzip.NewReader(r)
	if err != nil {
		return "", fmt.Errorf("pack: not a gzipped tarball: %w", err)
	}
	defer gz.Close()

	entries, err := readTarEntries(tar.NewReader(gz))
	if err != nil {
		return "", err
	}
	manifestData, ok := entries[plugin.ManifestFileName]
	if !ok {
		return "", fmt.Errorf("pack: tarball has no %s at the root", plugin.ManifestFileName)
	}
	mf, err := parseManifest(manifestData)
	if err != nil {
		return "", fmt.Errorf("pack: manifest in tarball: %w", err)
	}
	if err := SafePluginName(mf.Metadata.Name); err != nil {
		return "", err
	}
	dest := filepath.Join(targetRoot, mf.Metadata.Name)
	if err := PrepareDest(dest, targetRoot, force); err != nil {
		return "", err
	}
	for name, data := range entries {
		full := filepath.Join(dest, name)
		if err := os.MkdirAll(filepath.Dir(full), 0o755); err != nil {
			return "", err
		}
		mode := os.FileMode(0o644)
		if strings.HasPrefix(name, "bin/") {
			mode = 0o755
		}
		if err := os.WriteFile(full, data, mode); err != nil {
			return "", err
		}
	}
	return mf.Metadata.Name, nil
}

// SafePluginName rejects a manifest.name that could escape the target
// dir. The schema already restricts names to ^[a-z][a-z0-9-]{1,38}[a-z0-9]$
// so this is defense in depth — a code path that skips validation
// slips a bad name in without the filesystem catching it.
func SafePluginName(name string) error {
	if name == "" {
		return fmt.Errorf("pack: manifest.metadata.name is empty")
	}
	if name != filepath.Base(name) || strings.ContainsAny(name, `/\`) || strings.HasPrefix(name, ".") {
		return fmt.Errorf("pack: refusing unsafe plugin name %q", name)
	}
	return nil
}

// PrepareDest ensures a fresh dest directory exists under targetRoot.
// Refuses to overwrite unless force is set; refuses symlinks
// unconditionally so RemoveAll never follows one.
func PrepareDest(dest, targetRoot string, force bool) error {
	absRoot, err := filepath.Abs(targetRoot)
	if err != nil {
		return fmt.Errorf("pack: resolve target: %w", err)
	}
	absDest, err := filepath.Abs(dest)
	if err != nil {
		return fmt.Errorf("pack: resolve dest: %w", err)
	}
	rel, err := filepath.Rel(absRoot, absDest)
	if err != nil || strings.HasPrefix(rel, "..") || rel == "." {
		return fmt.Errorf("pack: destination %s escapes target %s", dest, targetRoot)
	}
	info, err := os.Lstat(dest)
	switch {
	case err == nil && info.Mode()&os.ModeSymlink != 0:
		return fmt.Errorf("pack: %s is a symlink; refusing to overwrite", dest)
	case err == nil && !info.IsDir():
		return fmt.Errorf("pack: %s exists and is not a directory", dest)
	case err == nil:
		if !force {
			return fmt.Errorf("pack: %s already exists; pass force=true to overwrite", dest)
		}
		if err := os.RemoveAll(dest); err != nil {
			return err
		}
	case !os.IsNotExist(err):
		return err
	}
	return os.MkdirAll(dest, 0o755)
}

// RemoveInstalled removes an installed plugin dir. Verifies the path
// resolves strictly inside pluginsRoot before touching it, so a
// mistyped name cannot delete unrelated content.
func RemoveInstalled(pluginsRoot, name string) error {
	if err := SafePluginName(name); err != nil {
		return err
	}
	dest := filepath.Join(pluginsRoot, name)
	absRoot, err := filepath.Abs(pluginsRoot)
	if err != nil {
		return err
	}
	absDest, err := filepath.Abs(dest)
	if err != nil {
		return err
	}
	rel, err := filepath.Rel(absRoot, absDest)
	if err != nil || strings.HasPrefix(rel, "..") || rel == "." {
		return fmt.Errorf("pack: uninstall destination %s escapes plugins root", dest)
	}
	info, err := os.Lstat(dest)
	if err != nil {
		return err
	}
	if info.Mode()&os.ModeSymlink != 0 {
		return fmt.Errorf("pack: %s is a symlink; refusing to remove", dest)
	}
	return os.RemoveAll(dest)
}

func readTarEntries(tr *tar.Reader) (map[string][]byte, error) {
	out := map[string][]byte{}
	for {
		hdr, err := tr.Next()
		if err == io.EOF {
			return out, nil
		}
		if err != nil {
			return nil, err
		}
		name := filepath.Clean(hdr.Name)
		if filepath.IsAbs(name) || strings.HasPrefix(name, "..") {
			return nil, fmt.Errorf("pack: unsafe path in tarball: %s", hdr.Name)
		}
		if hdr.Typeflag != tar.TypeReg {
			continue
		}
		data, err := io.ReadAll(tr)
		if err != nil {
			return nil, err
		}
		out[filepath.ToSlash(name)] = data
	}
}

func parseManifest(data []byte) (*plugin.Manifest, error) {
	return plugin.Parse(bytes.NewReader(data))
}
