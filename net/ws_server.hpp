#pragma once

// A deliberately small HTTP + WebSocket server (POSIX sockets + poll), used
// to serve the dashboard and exchange JSON messages with it. Several
// WebSocket clients may be connected (e.g. the browser dashboard and the
// MuJoCo bridge); messages are broadcast to all of them.
//
// Everything it receives is untrusted input: requests and frames are size-
// limited, file paths are confined to the static directory, and malformed
// data closes the connection. It never blocks: writes are buffered and
// telemetry is dropped if a client cannot keep up.
//
// Portable POSIX code, so it can later run on QNX as the network gateway
// (link with -lsocket there).

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace surgisafe::net {

class WsServer {
public:
    WsServer() = default;
    ~WsServer();
    WsServer(const WsServer&) = delete;
    WsServer& operator=(const WsServer&) = delete;

    // Listens on `bind_addr:port` and serves files from `static_dir`.
    bool listen(const char* bind_addr, std::uint16_t port,
                const std::string& static_dir);

    // Handles network activity for up to `timeout_ms` milliseconds.
    void poll(int timeout_ms);

    // Queues a text message to every WebSocket client. A client whose send
    // buffer is full misses this message (telemetry is droppable). Returns
    // false if there is no client.
    bool send_text(const std::string& message);

    bool has_client() const { return ws_index() >= 0; }

    std::function<void()> on_open;
    std::function<void(const std::string&)> on_message;

private:
    struct Conn {
        int fd = -1;
        bool websocket = false;
        bool close_after_flush = false;
        std::string in;
        std::string out;
    };

    void accept_new();
    void handle_readable(Conn& c);
    void handle_http(Conn& c);
    void handle_frames(Conn& c);
    void flush(Conn& c);
    void close_conn(Conn& c);
    int ws_index() const;
    static std::string frame(std::uint8_t opcode, const std::string& payload);

    int listen_fd_ = -1;
    std::string static_dir_;
    std::vector<Conn> conns_;
};

} // namespace surgisafe::net
