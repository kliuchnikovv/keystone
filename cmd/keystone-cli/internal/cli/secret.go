package cli

import (
	"bufio"
	"fmt"
	"net/url"
	"os"
	"strings"
)

// SecretRoot dispatches "keystone secret <verb> …".
//
// Values never travel through argv — an operator's shell history is
// not a credential store. `set` reads a single line from stdin.
// Hidden-echo interactive input (like ssh's password prompt) wants
// golang.org/x/term; we intentionally keep the CLI dep-free for
// v1 and rely on the shell to redirect a file:
//
//	printf '%s' "$MY_TOKEN" | keystone secret set matter.fabric-key
//
// A follow-on can add x/term-based hidden prompts.
func SecretRoot(args []string) error {
	if len(args) == 0 {
		return usageErr("secret: verb required (set|unset|list|rotate)")
	}
	switch args[0] {
	case "set":
		return secretSet(args[1:])
	case "unset":
		return secretUnset(args[1:])
	case "list", "ls":
		return secretList(args[1:])
	case "rotate":
		return secretRotate(args[1:])
	default:
		return usageErr("secret: unknown verb %q", args[0])
	}
}

func secretSet(args []string) error {
	if len(args) == 0 {
		return usageErr("secret set <key>  (value read from stdin)")
	}
	value, err := readSecretFromStdin()
	if err != nil {
		return err
	}
	var resp map[string]any
	if err := Put("/secrets/"+url.PathEscape(args[0]), map[string]any{"value": value}, &resp); err != nil {
		return err
	}
	Render(resp)
	return nil
}

func secretUnset(args []string) error {
	if len(args) == 0 {
		return usageErr("secret unset <key>")
	}
	var resp map[string]any
	if err := Delete("/secrets/"+url.PathEscape(args[0]), &resp); err != nil {
		return err
	}
	Render(resp)
	return nil
}

func secretList(_ []string) error {
	// The server returns names, never values. If it ever returns a
	// value field, refuse to render it — belt-and-braces against a
	// bug that would leak on stdout.
	var out struct {
		Secrets []struct {
			Name string `json:"name"`
		} `json:"secrets"`
	}
	if err := Get("/secrets", &out); err != nil {
		return err
	}
	if G().Output == "json" {
		Render(out.Secrets)
		return nil
	}
	rows := make([][]string, 0, len(out.Secrets))
	for _, s := range out.Secrets {
		rows = append(rows, []string{s.Name})
	}
	Render(Table{Header: []string{"NAME"}, Rows: rows})
	return nil
}

func secretRotate(args []string) error {
	if len(args) == 0 {
		return usageErr("secret rotate <key>")
	}
	var resp map[string]any
	if err := Post("/secrets/"+url.PathEscape(args[0])+"/rotate", nil, &resp); err != nil {
		return err
	}
	Render(resp)
	return nil
}

// readSecretFromStdin reads one line (or the whole stdin, whichever
// comes first) and trims a single trailing newline. If stdin is a
// tty, prints a hint so the operator knows what's expected.
func readSecretFromStdin() (string, error) {
	if isTTY(os.Stdin) {
		fmt.Fprintln(os.Stderr, "reading secret from stdin; type value and press Ctrl-D:")
	}
	sc := bufio.NewScanner(os.Stdin)
	// Big buffer for tokens like PEM blocks.
	sc.Buffer(make([]byte, 0, 4096), 1<<20)
	var parts []string
	for sc.Scan() {
		parts = append(parts, sc.Text())
	}
	if err := sc.Err(); err != nil {
		return "", err
	}
	v := strings.Join(parts, "\n")
	if v == "" {
		return "", fmt.Errorf("secret value is empty")
	}
	return v, nil
}

