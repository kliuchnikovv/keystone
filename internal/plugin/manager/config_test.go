package manager

import (
	"encoding/json"
	"strings"
	"testing"

	"github.com/kliuchnikovv/keystone/internal/plugin"
)

func TestValidateConfig_NoSchemaAcceptsAnything(t *testing.T) {
	mf := &plugin.Manifest{}
	if err := validateConfig(mf, json.RawMessage(`{"foo": "bar"}`)); err != nil {
		t.Fatalf("unexpected: %v", err)
	}
}

func TestValidateConfig_AcceptsValid(t *testing.T) {
	mf := &plugin.Manifest{}
	mf.Spec.Config = &plugin.ConfigSpec{Schema: `{
		"type": "object",
		"properties": {"port": {"type": "integer", "minimum": 1024}},
		"required": ["port"]
	}`}
	if err := validateConfig(mf, json.RawMessage(`{"port": 5580}`)); err != nil {
		t.Fatalf("unexpected: %v", err)
	}
}

func TestValidateConfig_RejectsInvalid(t *testing.T) {
	mf := &plugin.Manifest{}
	mf.Spec.Config = &plugin.ConfigSpec{Schema: `{
		"type": "object",
		"properties": {"port": {"type": "integer", "minimum": 1024}},
		"required": ["port"]
	}`}
	err := validateConfig(mf, json.RawMessage(`{"port": 80}`))
	if err == nil {
		t.Fatal("expected error for port below minimum")
	}
	if !strings.Contains(err.Error(), "port") {
		t.Fatalf("error should mention the failing field, got %v", err)
	}
}
