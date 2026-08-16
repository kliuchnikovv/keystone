.PHONY: build build-grpc run test tidy fmt lint clean proto proto-check \
	build-plugin-validate validate-manifests

BINARY := bin/keystone
PLUGIN_VALIDATE := bin/keystone-plugin-validate
PROTO_SRC := $(shell find proto -name '*.proto' 2>/dev/null)
PROTO_OUT := gen/go/keystone/v1

# Default build: no gRPC, no external deps needed. Builds both binaries.
build:
	@mkdir -p bin
	go build -o $(BINARY) ./cmd/keystone

# gRPC-enabled build.
build-grpc: proto
	@mkdir -p bin
	go build -tags=grpc -o $(BINARY) ./cmd/keystone

run: build
	./$(BINARY)

build-plugin-validate:
	@mkdir -p bin
	go build -o $(PLUGIN_VALIDATE) ./cmd/keystone-plugin-validate

# Manifest gate for CI. Point it at plugin directories with MANIFESTS=...;
# by default it re-checks the reference manifests shipped with the validator.
MANIFESTS ?= internal/plugin/testdata/valid/*.yaml
validate-manifests: build-plugin-validate
	./$(PLUGIN_VALIDATE) $(MANIFESTS)

test:
	go test ./...

proto: $(PROTO_SRC)
	@mkdir -p $(PROTO_OUT)
	@if command -v buf >/dev/null; then \
		echo "generating with buf"; \
		buf generate; \
	elif command -v protoc >/dev/null; then \
		echo "generating with protoc"; \
		protoc -I proto -I /usr/include \
			--go_out=gen/go --go_opt=paths=source_relative \
			--go-grpc_out=gen/go --go-grpc_opt=paths=source_relative \
			$(PROTO_SRC); \
	else \
		echo "install buf (recommended) or protoc + protoc-gen-go + protoc-gen-go-grpc"; \
		exit 1; \
	fi

proto-check:
	protoc --descriptor_set_out=/dev/null -I proto -I /usr/include $(PROTO_SRC)

tidy:
	go mod tidy

fmt:
	gofmt -s -w .
	go vet ./...

lint:
	@if ! command -v staticcheck >/dev/null; then \
		echo "installing staticcheck..."; \
		go install honnef.co/go/tools/cmd/staticcheck@latest; \
	fi
	staticcheck ./...

clean:
	rm -rf bin coverage.out gen/
