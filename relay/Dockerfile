# Keystone relay, static binary on a minimal base.
#
#   docker build -t keystone-relay relay/
#   docker run -d --name keystone-relay --restart unless-stopped \
#     -e SECRET="$(openssl rand -hex 32)" -p 9090:9090 keystone-relay
#
# Put TLS in front (scripts/Caddyfile) or pass --tls-cert/--tls-key.

FROM golang:1.24-alpine AS build
WORKDIR /src
COPY go.mod ./
COPY . .
# No module graph: nothing to download, the build works offline.
RUN CGO_ENABLED=0 go build -trimpath -ldflags="-s -w" -o /out/keystone-relay .

FROM alpine:3.20
RUN adduser -D -H -u 10001 relay
COPY --from=build /out/keystone-relay /usr/local/bin/keystone-relay
USER relay
EXPOSE 9090
ENTRYPOINT ["/usr/local/bin/keystone-relay"]
CMD ["--port", "9090"]