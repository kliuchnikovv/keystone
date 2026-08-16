module github.com/kliuchnikovv/keystone

go 1.23

// gRPC/protobuf dependencies are added automatically by `go mod tidy` after
// `make proto` generates code under gen/.
//
// The base build (no -tags=grpc) stays deliberately thin. The only external
// packages it pulls in serve the plugin manifest contract (internal/plugin):
// yaml.v3 for parsing plus source positions, and jsonschema for validating
// plugin.yaml against internal/plugin/manifest_schema.json. Both are pure Go.

require (
	github.com/coder/websocket v1.8.15
	github.com/kliuchnikovv/keystone-api/sidecar v0.0.0-00010101000000-000000000000
	github.com/santhosh-tekuri/jsonschema/v6 v6.0.3
	golang.org/x/text v0.14.0
	gopkg.in/yaml.v3 v3.0.1
)

replace github.com/kliuchnikovv/keystone-api/sidecar => /Users/kliuchnikovv/go/src/github.com/kliuchnikovv/keystone-api/sidecar
