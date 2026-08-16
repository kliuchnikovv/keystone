package cli

import (
	"encoding/json"
	"os"
)

// isTTY returns true when f is attached to a terminal. Used by
// secret.go to decide whether to print a "type value" hint.
func isTTY(f *os.File) bool {
	fi, err := f.Stat()
	if err != nil {
		return false
	}
	return fi.Mode()&os.ModeCharDevice != 0
}

// jsonUnmarshalStrict is thin wrapper so tests can swap it later. The
// strictness knob is a placeholder — encoding/json is permissive by
// default and DisallowUnknownFields would be too strict for a
// hand-written rule.
func jsonUnmarshalStrict(data []byte, v any) error {
	return json.Unmarshal(data, v)
}
