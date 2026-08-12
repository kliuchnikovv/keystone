package dirigera

import (
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
)

// Config is what we persist between runs. Stored in <data-dir>/dirigera.json.
type Config struct {
	Host  string `json:"host"`
	Token string `json:"token"`
}

// LoadConfig reads config from the given data directory.
// Returns os.ErrNotExist if the file is absent so callers can start unconfigured.
func LoadConfig(dataDir string) (*Config, error) {
	p := filepath.Join(dataDir, "dirigera.json")
	data, err := os.ReadFile(p)
	if err != nil {
		return nil, err
	}
	var c Config
	if err := json.Unmarshal(data, &c); err != nil {
		return nil, fmt.Errorf("parse %s: %w", p, err)
	}
	if c.Host == "" || c.Token == "" {
		return nil, errors.New("dirigera.json missing host or token")
	}
	return &c, nil
}

// SaveConfig writes config to <data-dir>/dirigera.json atomically.
func SaveConfig(dataDir string, c *Config) error {
	if err := os.MkdirAll(dataDir, 0o755); err != nil {
		return err
	}
	p := filepath.Join(dataDir, "dirigera.json")
	data, err := json.MarshalIndent(c, "", "  ")
	if err != nil {
		return err
	}
	tmp := p + ".tmp"
	if err := os.WriteFile(tmp, data, 0o600); err != nil {
		return err
	}
	return os.Rename(tmp, p)
}
