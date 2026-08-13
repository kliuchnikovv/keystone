# syntax=docker/dockerfile:1.7

FROM golang:1.23-bookworm AS build
WORKDIR /src
COPY go.mod go.sum ./
RUN go mod download
COPY cmd ./cmd
COPY internal ./internal
# gen/ (generated protobuf) is optional — only produced when `make proto`
# is run. If your build path needs gRPC, generate protos first and remove
# .dockerignore's exclusion.
RUN CGO_ENABLED=0 go build -o /out/keystone ./cmd/keystone

FROM debian:bookworm-slim
WORKDIR /app
RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates \
    && rm -rf /var/lib/apt/lists/*
COPY --from=build /out/keystone /usr/local/bin/keystone
# Copy optional static UI so the container can serve /ui/* out of the box.
COPY site /app/site
VOLUME ["/data"]
EXPOSE 7777
ENV KEYSTONE_DATA=/data
ENV KEYSTONE_ADDR=:7777
ENV KEYSTONE_MATTER_SIDECAR=ws://localhost:5580
ENV KEYSTONE_UI_DIR=/app/site
ENTRYPOINT ["/bin/sh","-c","exec /usr/local/bin/keystone \
    -data \"$KEYSTONE_DATA\" \
    -addr \"$KEYSTONE_ADDR\" \
    -matter-sidecar \"$KEYSTONE_MATTER_SIDECAR\" \
    -ui-dir \"$KEYSTONE_UI_DIR\" \
    -demo=false"]
