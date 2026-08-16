package main

import (
	"archive/tar"
	"bytes"
	"compress/gzip"
	"flag"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"strings"

	"github.com/kliuchnikovv/keystone/internal/plugin"
)

func runInstall(args []string) error {
	fs := flag.NewFlagSet("install", flag.ContinueOnError)
	dir := fs.String("dir", "", "plugin source directory (mutually exclusive with --tarball)")
	tarball := fs.String("tarball", "", "packed plugin tarball produced by `keystone-plugin publish`")
	target := fs.String("target", "", "plugins directory the plugin lands under — usually keystone's -plugins-dir (required)")
	force := fs.Bool("force", false, "overwrite an existing plugin dir")
	reload := fs.String("reload", "", "keystone URL to POST /plugins/discover after install, e.g. http://localhost:7777")
	if err := fs.Parse(args); err != nil {
		return err
	}
	if *target == "" {
		return fmt.Errorf("--target is required — point it at keystone's -plugins-dir")
	}
	if (*dir == "" && *tarball == "") || (*dir != "" && *tarball != "") {
		return fmt.Errorf("exactly one of --dir or --tarball must be set")
	}

	var name string
	var err error
	if *dir != "" {
		name, err = installFromDir(*dir, *target, *force)
	} else {
		name, err = installFromTarball(*tarball, *target, *force)
	}
	if err != nil {
		return err
	}
	fmt.Printf("installed %s at %s\n", name, filepath.Join(*target, name))

	if *reload != "" {
		if err := notifyDiscover(*reload); err != nil {
			fmt.Fprintf(os.Stderr, "warning: install succeeded but /plugins/discover on %s failed: %v\n", *reload, err)
		} else {
			fmt.Println("reloaded", *reload)
		}
	}
	return nil
}

func installFromDir(src, targetRoot string, force bool) (string, error) {
	mf, err := plugin.ParseFile(filepath.Join(src, plugin.ManifestFileName))
	if err != nil {
		return "", fmt.Errorf("install: %w", err)
	}
	if err := safePluginName(mf.Metadata.Name); err != nil {
		return "", err
	}
	dest := filepath.Join(targetRoot, mf.Metadata.Name)
	if err := prepareDest(dest, targetRoot, force); err != nil {
		return "", err
	}

	// Copy the manifest and the four blessed subtrees. Anything else in
	// the source dir (Go source, .git, scratch files) stays behind —
	// the runtime does not need it.
	if err := copyFile(filepath.Join(src, plugin.ManifestFileName), filepath.Join(dest, plugin.ManifestFileName), 0o644); err != nil {
		return "", err
	}
	for _, sub := range packedPaths {
		if err := copyTree(filepath.Join(src, sub), filepath.Join(dest, sub)); err != nil {
			return "", fmt.Errorf("install: copy %s: %w", sub, err)
		}
	}
	return mf.Metadata.Name, nil
}

func installFromTarball(tarPath, targetRoot string, force bool) (string, error) {
	f, err := os.Open(tarPath)
	if err != nil {
		return "", err
	}
	defer f.Close()
	gz, err := gzip.NewReader(f)
	if err != nil {
		return "", fmt.Errorf("install: not a gzipped tarball: %w", err)
	}
	defer gz.Close()

	// First pass: buffer entries in memory and read the manifest to
	// learn the target name. Plugin tarballs are small enough that
	// this is fine; if a plugin ever grows big enough for this to
	// matter, a two-pass tarball read would replace this.
	entries, err := readTarEntries(tar.NewReader(gz))
	if err != nil {
		return "", err
	}
	manifestData, ok := entries[plugin.ManifestFileName]
	if !ok {
		return "", fmt.Errorf("install: tarball has no plugin.yaml at the root")
	}
	mf, err := plugin.Parse(bytes.NewReader(manifestData))
	if err != nil {
		return "", fmt.Errorf("install: manifest in tarball: %w", err)
	}
	if err := safePluginName(mf.Metadata.Name); err != nil {
		return "", err
	}
	dest := filepath.Join(targetRoot, mf.Metadata.Name)
	if err := prepareDest(dest, targetRoot, force); err != nil {
		return "", err
	}
	for name, data := range entries {
		full := filepath.Join(dest, name)
		if err := os.MkdirAll(filepath.Dir(full), 0o755); err != nil {
			return "", err
		}
		mode := os.FileMode(0o644)
		// The tarball preserves the exec bit for entrypoints; carry it
		// through so the manager can spawn the child right away.
		if strings.HasPrefix(name, "bin/") {
			mode = 0o755
		}
		if err := os.WriteFile(full, data, mode); err != nil {
			return "", err
		}
	}
	return mf.Metadata.Name, nil
}

// safePluginName rejects a manifest.name that could escape the target
// dir. The schema already restricts names to ^[a-z][a-z0-9-]{1,38}[a-z0-9]$
// so this is defense in depth — if a future schema change or a code path
// that skips validation slips one in, we refuse to touch the filesystem
// with it.
func safePluginName(name string) error {
	if name == "" {
		return fmt.Errorf("install: manifest.metadata.name is empty")
	}
	if name != filepath.Base(name) || strings.ContainsAny(name, `/\`) || strings.HasPrefix(name, ".") {
		return fmt.Errorf("install: refusing unsafe plugin name %q", name)
	}
	return nil
}

// prepareDest ensures a fresh target directory exists. Overwrites an
// existing one only when force is set; the default is to refuse and
// let the caller decide. Before any destructive step, it confirms
// dest is a plain directory strictly under targetRoot — a symlink or
// a traversal-escaping name is refused rather than followed.
func prepareDest(dest, targetRoot string, force bool) error {
	// Resolve targetRoot up front so a comparison later is against a
	// stable absolute path.
	absRoot, err := filepath.Abs(targetRoot)
	if err != nil {
		return fmt.Errorf("install: resolve target: %w", err)
	}
	absDest, err := filepath.Abs(dest)
	if err != nil {
		return fmt.Errorf("install: resolve dest: %w", err)
	}
	rel, err := filepath.Rel(absRoot, absDest)
	if err != nil || strings.HasPrefix(rel, "..") || rel == "." {
		return fmt.Errorf("install: destination %s escapes target %s", dest, targetRoot)
	}

	// If dest exists, it must be a real directory, not a symlink pointing
	// elsewhere — otherwise RemoveAll would follow the link and nuke the
	// wrong tree.
	info, err := os.Lstat(dest)
	switch {
	case err == nil && info.Mode()&os.ModeSymlink != 0:
		return fmt.Errorf("install: %s is a symlink; refusing to overwrite", dest)
	case err == nil && !info.IsDir():
		return fmt.Errorf("install: %s exists and is not a directory", dest)
	case err == nil:
		if !force {
			return fmt.Errorf("install: %s already exists; pass --force to overwrite", dest)
		}
		if err := os.RemoveAll(dest); err != nil {
			return err
		}
	case !os.IsNotExist(err):
		return err
	}
	return os.MkdirAll(dest, 0o755)
}

func copyFile(src, dst string, mode os.FileMode) error {
	in, err := os.Open(src)
	if err != nil {
		return err
	}
	defer in.Close()
	if err := os.MkdirAll(filepath.Dir(dst), 0o755); err != nil {
		return err
	}
	out, err := os.OpenFile(dst, os.O_WRONLY|os.O_CREATE|os.O_TRUNC, mode)
	if err != nil {
		return err
	}
	defer out.Close()
	_, err = io.Copy(out, in)
	return err
}

func copyTree(src, dst string) error {
	info, err := os.Stat(src)
	if err != nil {
		if os.IsNotExist(err) {
			return nil
		}
		return err
	}
	if !info.IsDir() {
		return nil
	}
	return filepath.Walk(src, func(path string, info os.FileInfo, err error) error {
		if err != nil {
			return err
		}
		rel, err := filepath.Rel(src, path)
		if err != nil {
			return err
		}
		target := filepath.Join(dst, rel)
		if info.IsDir() {
			return os.MkdirAll(target, 0o755)
		}
		mode := os.FileMode(0o644)
		if info.Mode()&0o111 != 0 {
			mode = 0o755
		}
		return copyFile(path, target, mode)
	})
}

// readTarEntries reads every regular file from a tar archive into a map
// keyed by header name. Directory entries are dropped (recreated
// implicitly by os.MkdirAll during write).
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
		// Refuse absolute or escaping paths — a malicious tarball must
		// not write outside the target.
		if filepath.IsAbs(name) || strings.HasPrefix(name, "..") {
			return nil, fmt.Errorf("install: unsafe path in tarball: %s", hdr.Name)
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

// notifyDiscover POSTs /plugins/discover on the given keystone base so
// a freshly installed plugin lands in the manager's view without a
// daemon restart. Non-200 responses become errors, but the install has
// already succeeded — the caller decides how loud to be.
func notifyDiscover(base string) error {
	u, err := url.Parse(base)
	if err != nil {
		return err
	}
	u.Path = "/plugins/discover"
	resp, err := http.Post(u.String(), "application/json", nil)
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	if resp.StatusCode >= 400 {
		body, _ := io.ReadAll(io.LimitReader(resp.Body, 4096))
		return fmt.Errorf("%s: %s", resp.Status, body)
	}
	return nil
}
