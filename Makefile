.PHONY: build run test tidy fmt lint clean

BINARY := bin/keystone

build:
	@mkdir -p bin
	go build -o $(BINARY) ./cmd/keystone

run: build
	./$(BINARY)

test:
	go test ./...

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
	rm -rf bin coverage.out
