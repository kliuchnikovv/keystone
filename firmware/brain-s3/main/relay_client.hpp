// Relay client: the brain connects out to a relay server for remote access.
// Speaks the same multiplexing protocol as the Node.js tunnel agent (app/tools/relay-tunnel.ts)
// but runs directly on the ESP32 — no intermediate device needed.
//
// Virtual clients (remote users who connect through the relay) appear in the main loop just
// like local WebSocket clients: Connected → Cmd/Req via next(), replies via send()/broadcast().
// Their fds are negative (kFdBase - cid) so the dispatch layer routes them here, not to appsrv.
//
// Console:
//   relay                              show status
//   relay <url> <secret>               save credentials to NVS, connect
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
