module github.com/kliuchnikovv/keystone

go 1.23

// gRPC/protobuf dependencies are added automatically by `go mod tidy` after
// `make proto` generates code under gen/. The base build (no -tags=grpc) has
// zero external dependencies.
