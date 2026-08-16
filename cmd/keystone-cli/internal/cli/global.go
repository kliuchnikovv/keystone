// Package cli implements the keystone CLI's group commands. Each
// group lives in its own file; this one holds shared helpers:
// global flags, HTTP client, output rendering, exit codes.
package cli

import (
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"os"
	"strings"
	"time"
)

// Global carries CLI-wide flags. main.go parses them once and hands
// them here via SetGlobal so every group reads the same value.
type Global struct {
	Host    string
	Output  string
	Verbose bool
}

var current Global

// SetGlobal replaces the process-wide config. Called once from main.
func SetGlobal(g Global) { current = g }

// G returns the current global config so tests and helpers can read
// it without importing main.
func G() Global { return current }

// Exit codes per docs/keystone-cli-spec.md §3.
type Code int

const (
	CodeOK         Code = 0
	CodeError      Code = 1
	CodeValidation Code = 2
	CodeNotFound   Code = 3
	CodeAuth       Code = 4
	CodeTimeout    Code = 5
	CodePlugin     Code = 6
	CodeConflict   Code = 7
)

// Exit is the last call every command makes. It maps error → code
// and calls os.Exit; nil is code 0.
func Exit(err error) {
	if err == nil {
		os.Exit(int(CodeOK))
	}
	if !current.Verbose {
		fmt.Fprintln(os.Stderr, err)
	} else {
		fmt.Fprintf(os.Stderr, "error: %+v\n", err)
	}
	var httpErr *HTTPError
	if errors.As(err, &httpErr) {
		os.Exit(int(mapHTTP(httpErr.Status)))
	}
	os.Exit(int(CodeError))
}

// HTTPError is what Get / Post / … return on a non-2xx response.
// Callers can errors.As to check the status without re-parsing.
type HTTPError struct {
	Status int
	Method string
	Path   string
	Body   string
}

func (e *HTTPError) Error() string {
	return fmt.Sprintf("%s %s: HTTP %d: %s", e.Method, e.Path, e.Status, strings.TrimSpace(e.Body))
}

func mapHTTP(status int) Code {
	switch {
	case status == 400:
		return CodeValidation
	case status == 401, status == 403:
		return CodeAuth
	case status == 404:
		return CodeNotFound
	case status == 408, status == 504:
		return CodeTimeout
	case status == 409:
		return CodeConflict
	default:
		return CodeError
	}
}

// -------- HTTP client --------

var httpClient = &http.Client{Timeout: 30 * time.Second}

// Get issues a GET to <host><path> and decodes the response body
// into out. A non-2xx response returns *HTTPError so Exit maps it
// to the right code.
func Get(path string, out any) error {
	return do(http.MethodGet, path, nil, out)
}

func Post(path string, body any, out any) error {
	return do(http.MethodPost, path, body, out)
}

func Put(path string, body any, out any) error {
	return do(http.MethodPut, path, body, out)
}

func Delete(path string, out any) error {
	return do(http.MethodDelete, path, nil, out)
}

func do(method, path string, body any, out any) error {
	var payload io.Reader
	if body != nil {
		raw, err := json.Marshal(body)
		if err != nil {
			return err
		}
		payload = strings.NewReader(string(raw))
	}
	req, err := http.NewRequest(method, current.Host+path, payload)
	if err != nil {
		return err
	}
	if body != nil {
		req.Header.Set("content-type", "application/json")
	}
	resp, err := httpClient.Do(req)
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	raw, _ := io.ReadAll(resp.Body)
	if resp.StatusCode >= 400 {
		return &HTTPError{Status: resp.StatusCode, Method: method, Path: path, Body: string(raw)}
	}
	if out == nil || len(raw) == 0 {
		return nil
	}
	return json.Unmarshal(raw, out)
}

// GetStream fires a GET whose body is an NDJSON stream. Each line
// yields one payload decoded into T; the callback returns false to
// stop reading.
func GetStream(path string, onLine func([]byte) bool) error {
	req, err := http.NewRequest(http.MethodGet, current.Host+path, nil)
	if err != nil {
		return err
	}
	// Streams override the default timeout — a long-running follow
	// must not die on 30s idle.
	client := &http.Client{}
	resp, err := client.Do(req)
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	if resp.StatusCode >= 400 {
		body, _ := io.ReadAll(io.LimitReader(resp.Body, 4096))
		return &HTTPError{Status: resp.StatusCode, Method: http.MethodGet, Path: path, Body: string(body)}
	}
	buf := make([]byte, 0, 4096)
	tmp := make([]byte, 4096)
	for {
		n, err := resp.Body.Read(tmp)
		if n > 0 {
			buf = append(buf, tmp[:n]...)
			for {
				i := indexByte(buf, '\n')
				if i < 0 {
					break
				}
				line := buf[:i]
				buf = buf[i+1:]
				if len(line) == 0 {
					continue
				}
				if !onLine(line) {
					return nil
				}
			}
		}
		if err == io.EOF {
			return nil
		}
		if err != nil {
			return err
		}
	}
}

func indexByte(b []byte, c byte) int {
	for i, v := range b {
		if v == c {
			return i
		}
	}
	return -1
}
