// Relay client: the brain connects out to a relay server for remote access.
// Speaks the same multiplexing protocol as the Node.js tunnel agent (app/tools/relay-tunnel.ts)
// but runs directly on the ESP32 — no intermediate device needed.
//
// Virtual clients (remote users who connect through the relay) appear in the main loop just
// like local WebSocket clients: Connected → Cmd/Req via next(), replies via send()/broadcast().
// Their fds are negative (kFdBase - cid) so the dispatch layer routes them here, not to appsrv.
//
// Threading: the WebSocket event handler only parses and queues. Everything the hub sends
// leaves through the outbound queue drained by tick() in the main loop — esp_websocket_client
// holds its internal lock while calling the event handler, so sending from inside the callback
// deadlocks against a concurrent send from the main loop.
//
// Console:
//   relay                              show status
//   relay <secret>                     connect to the default relay
//   relay <host> <secret>              connect to a custom relay
//   relay clear                        disconnect, erase NVS credentials
#pragma once

#include <cstddef>
#include <cstdint>

#include "app_server.hpp"

namespace relay {

constexpr int kFdBase = -100;

inline bool is_relay_fd(int fd) { return fd <= kFdBase; }

using Inbound = appsrv::Inbound;
using HubInfoFn = appsrv::HubInfoFn;

bool configure(const char* url, const char* secret);
void clear_config();
void describe(char* buf, std::size_t cap);
// The configured relay URL ("" when there is none). The secret is never handed out.
bool configured_url(char* out, std::size_t cap);

// The identity baked into the firmware when the box is flashed (main/relay_identity.h):
// the relay this hub belongs to, the id it uses there, and a secret of its own.
// Empty strings when the firmware was not flashed with one.
const char* baked_host();
const char* baked_id();
const char* baked_secret();

const char* default_host();
const char* default_id();
// Build "wss://<host>/tunnel/<id>". Null/empty host → the default relay,
// null/empty id → "home". Returns snprintf result (< 0 or >= cap on overflow).
int build_url(char* out, std::size_t cap, const char* host, const char* id);

bool start(const char* token, HubInfoFn info);
void stop();
void set_token(const char* token);
bool running();
bool connected();
void tick(std::uint32_t now_ms);

bool next(Inbound& out);
void send(int fd, const char* data, std::size_t len);
void broadcast(const char* data, std::size_t len);
std::size_t client_count();

}  // namespace relay
