// App protocol transport on the brain (docs/app-protocol.md): an esp_http_server
// WebSocket endpoint at /ws. It owns sockets, the handshake (token, lockout, client
// cap, hello timeout) and frame I/O, and nothing else: it never touches core
// objects. Decoded commands reach the main loop through next(); everything the main
// loop wants to say goes out through send()/broadcast(), which hand the frame to the
// server task (all socket writes happen there, so they are serialized).
//
// Stage 3: plain ws:// on port 8080 (development; TLS is stage 5).
#pragma once

#include <cstddef>
#include <cstdint>

#include "core/app_protocol.hpp"

namespace appsrv {

inline constexpr std::size_t kMaxClients = 3;  // authenticated; heap budget, see the spec

// What the server task hands to the main loop.
struct Inbound {
  enum class Kind : std::uint8_t {
    Connected,  // a client finished the handshake: send it the snapshot
    Cmd,        // a (syntactically valid) command
    Req,        // a management request: `text` is its raw JSON frame
  };
  Kind kind;
  int fd;
  std::int32_t id;
  std::int64_t entity;
  std::int64_t attr;
  core::app::Scalar value;
  // Req only: a malloc'd copy of the frame (the main loop parses it, then free()s it).
  char* text;
  std::size_t len;
};

// Fills the `welcome` hub object; called in the server task. Strings must stay valid
// until the next call.
using HubInfoFn = void (*)(core::app::HubInfo& out);

// Start (or restart with a new token) the server. Token: 8..64 characters.
bool start(const char* token, HubInfoFn info);
void stop();
bool running();

// Main loop side.
bool next(Inbound& out);                              // non-blocking
void send(int fd, const char* data, std::size_t len);  // async; may drop a stuck client
void broadcast(const char* data, std::size_t len);     // to every authenticated client
void close_client(int fd);
void tick(std::uint32_t now_ms);                       // handshake timeouts
std::size_t client_count();                            // authenticated clients
int port();

}  // namespace appsrv
