#pragma once

// Minimal SHA-1 and base64, only for the WebSocket opening handshake
// (RFC 6455 section 4.2.2). SHA-1 is not used for any security purpose.

#include <array>
#include <cstdint>
#include <string>

namespace surgisafe::net {

std::array<std::uint8_t, 20> sha1(const std::string& data);
std::string base64(const std::uint8_t* data, std::size_t len);

// Sec-WebSocket-Accept value for a client's Sec-WebSocket-Key.
std::string websocket_accept(const std::string& client_key);

} // namespace surgisafe::net
