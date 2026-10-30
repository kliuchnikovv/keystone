# The Keystone relay as a container - for Railway or any Docker host.
#
# Build context is the repository root:
#   docker build -f relay.Dockerfile -t keystone-relay .
#   docker run --rm -p 9090:9090 -e RELAY_SECRET=<secret> keystone-relay
#
# The port and the secrets come from the environment (Railway sets PORT itself
# and passes RELAY_MASTER_SECRET or RELAY_SECRET as a service variable), so the
# container needs no arguments.
#
# RELAY_MASTER_SECRET is the one to use: the relay derives the secret of every
# hub from it, so a new home needs nothing added here. RELAY_SECRET (one secret
# for every hub) and RELAY_HUB_SECRETS ("id:secret,...") are the older ways.
#
# The relay is Go (relay/): one static binary, so the image carries no runtime
# at all. See docs/relay-security.md for the threat model.
FROM golang:1-alpine AS build

WORKDIR /src
COPY relay/go.mod relay/go.sum ./
RUN go mod download
COPY relay/ ./
RUN CGO_ENABLED=0 GOOS=linux go build -trimpath -ldflags="-s -w" -o /out/relay .

FROM alpine:3.20
RUN adduser -D -u 10001 relay
COPY --from=build /out/relay /usr/local/bin/relay
USER relay
ENV PORT=9090
EXPOSE 9090
ENTRYPOINT ["/usr/local/bin/relay"]
