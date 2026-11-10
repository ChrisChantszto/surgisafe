#include "net/ws_server.hpp"
#include "net/sha1.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

namespace surgisafe::net {

namespace {

constexpr std::size_t kMaxRequest = 16 * 1024;
constexpr std::size_t kMaxFrame = 64 * 1024;
constexpr std::size_t kMaxOutBuffer = 512 * 1024;

bool set_nonblocking(int fd) {
    const int flags = fcntl(fd, F_GETFL, 0);
    return flags != -1 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) != -1;
}

std::string lower(std::string s) {
    for (char& ch : s) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return s;
}

// Returns the value of an HTTP header (case-insensitive name), or "".
std::string header(const std::string& request, const std::string& name) {
    std::istringstream lines(request);
    std::string line;
    const std::string want = lower(name) + ":";
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (lower(line.substr(0, want.size())) == want) {
            std::string v = line.substr(want.size());
            const auto b = v.find_first_not_of(" \t");
            const auto e = v.find_last_not_of(" \t");
            return b == std::string::npos ? "" : v.substr(b, e - b + 1);
        }
    }
    return "";
}

const char* content_type(const std::string& path) {
    auto ends = [&](const char* ext) {
        const std::size_t n = std::strlen(ext);
        return path.size() >= n && path.compare(path.size() - n, n, ext) == 0;
    };
    if (ends(".html")) return "text/html; charset=utf-8";
    if (ends(".js") || ends(".mjs")) return "text/javascript; charset=utf-8";
    if (ends(".css")) return "text/css; charset=utf-8";
    if (ends(".svg")) return "image/svg+xml";
    if (ends(".json")) return "application/json";
    return "application/octet-stream";
}

std::string http_response(int status, const char* reason, const char* type,
                          const std::string& body) {
    std::ostringstream r;
    r << "HTTP/1.1 " << status << ' ' << reason << "\r\n"
      << "Content-Type: " << type << "\r\n"
      << "Content-Length: " << body.size() << "\r\n"
      << "Cache-Control: no-store\r\n"
      << "X-Content-Type-Options: nosniff\r\n"
      << "Connection: close\r\n\r\n"
      << body;
    return r.str();
}

} // namespace

WsServer::~WsServer() {
    for (Conn& c : conns_) close_conn(c);
    if (listen_fd_ != -1) ::close(listen_fd_);
}

bool WsServer::listen(const char* bind_addr, std::uint16_t port,
                      const std::string& static_dir) {
    static_dir_ = static_dir;
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ == -1) {
        std::perror("socket");
        return false;
    }
    const int one = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, bind_addr, &addr.sin_addr) != 1) {
        std::fprintf(stderr, "bad bind address %s\n", bind_addr);
        return false;
    }
    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) ==
            -1 ||
        ::listen(listen_fd_, 8) == -1 || !set_nonblocking(listen_fd_)) {
        std::perror("bind/listen");
        return false;
    }
    return true;
}

int WsServer::ws_index() const {
    for (std::size_t i = 0; i < conns_.size(); ++i) {
        if (conns_[i].websocket && conns_[i].fd != -1 &&
            !conns_[i].close_after_flush) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void WsServer::poll(int timeout_ms) {
    std::vector<pollfd> fds;
    fds.push_back({listen_fd_, POLLIN, 0});
    for (const Conn& c : conns_) {
        short events = POLLIN;
        if (!c.out.empty()) events |= POLLOUT;
        fds.push_back({c.fd, events, 0});
    }
    if (::poll(fds.data(), static_cast<nfds_t>(fds.size()), timeout_ms) <= 0) {
        return;
    }
    if (fds[0].revents & POLLIN) accept_new();
    // conns_ may have grown in accept_new(); only service the polled ones.
    for (std::size_t i = 1; i < fds.size(); ++i) {
        Conn& c = conns_[i - 1];
        if (c.fd == -1) continue;
        if (fds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            close_conn(c);
            continue;
        }
        if (fds[i].revents & POLLIN) handle_readable(c);
        if (c.fd != -1 && (fds[i].revents & POLLOUT)) flush(c);
    }
    conns_.erase(std::remove_if(conns_.begin(), conns_.end(),
                                [](const Conn& c) { return c.fd == -1; }),
                 conns_.end());
}

void WsServer::accept_new() {
    while (true) {
        const int fd = ::accept(listen_fd_, nullptr, nullptr);
        if (fd == -1) return;
        if (!set_nonblocking(fd) || conns_.size() >= 16) {
            ::close(fd);
            continue;
        }
        Conn c;
        c.fd = fd;
        conns_.push_back(std::move(c));
    }
}

void WsServer::handle_readable(Conn& c) {
    char buf[8192];
    while (true) {
        const ssize_t n = ::recv(c.fd, buf, sizeof buf, 0);
        if (n > 0) {
            c.in.append(buf, static_cast<std::size_t>(n));
            if (c.in.size() > kMaxRequest + kMaxFrame) {
                close_conn(c);
                return;
            }
            continue;
        }
        if (n == 0) {
            close_conn(c);
            return;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        if (errno == EINTR) continue;
        close_conn(c);
        return;
    }
    if (c.websocket) {
        handle_frames(c);
    } else if (c.in.find("\r\n\r\n") != std::string::npos) {
        handle_http(c);
    } else if (c.in.size() > kMaxRequest) {
        close_conn(c);
    }
}

void WsServer::handle_http(Conn& c) {
    const std::string request = c.in.substr(0, c.in.find("\r\n\r\n") + 4);
    c.in.erase(0, request.size());

    std::istringstream first(request);
    std::string method, path;
    first >> method >> path;

    if (lower(header(request, "Upgrade")) == "websocket") {
        const std::string key = header(request, "Sec-WebSocket-Key");
        if (method != "GET" || key.empty() || key.size() > 64) {
            c.out += http_response(400, "Bad Request", "text/plain", "bad\n");
            c.close_after_flush = true;
            flush(c);
            return;
        }
        c.out += "HTTP/1.1 101 Switching Protocols\r\n"
                 "Upgrade: websocket\r\nConnection: Upgrade\r\n"
                 "Sec-WebSocket-Accept: " +
                 websocket_accept(key) + "\r\n\r\n";
        c.websocket = true;
        flush(c);
        if (on_open) on_open();
        if (!c.in.empty()) handle_frames(c);
        return;
    }

    if (method != "GET") {
        c.out += http_response(405, "Method Not Allowed", "text/plain", "");
    } else {
        if (const auto q = path.find('?'); q != std::string::npos) {
            path.erase(q);
        }
        if (path == "/") path = "/index.html";
        // Confine requests to the static directory.
        const bool safe = path.size() > 1 && path[0] == '/' &&
                          path.find("..") == std::string::npos &&
                          path.find('\\') == std::string::npos;
        std::ifstream file;
        if (safe) file.open(static_dir_ + path, std::ios::binary);
        if (safe && file) {
            std::ostringstream body;
            body << file.rdbuf();
            c.out += http_response(200, "OK", content_type(path), body.str());
        } else {
            c.out += http_response(404, "Not Found", "text/plain",
                                   "not found\n");
        }
    }
    c.close_after_flush = true;
    flush(c);
}

void WsServer::handle_frames(Conn& c) {
    while (c.fd != -1) {
        if (c.in.size() < 2) return;
        const auto byte = [&](std::size_t i) {
            return static_cast<std::uint8_t>(c.in[i]);
        };
        const std::uint8_t opcode = byte(0) & 0x0F;
        const bool masked = byte(1) & 0x80;
        std::uint64_t len = byte(1) & 0x7F;
        std::size_t pos = 2;
        if (len == 126) {
            if (c.in.size() < 4) return;
            len = (static_cast<std::uint64_t>(byte(2)) << 8) | byte(3);
            pos = 4;
        } else if (len == 127) {
            if (c.in.size() < 10) return;
            len = 0;
            for (int i = 0; i < 8; ++i) len = (len << 8) | byte(2 + i);
            pos = 10;
        }
        // Clients must mask, and we do not accept large frames.
        if (!masked || len > kMaxFrame) {
            close_conn(c);
            return;
        }
        if (c.in.size() < pos + 4 + len) return;
        std::uint8_t mask[4];
        for (int i = 0; i < 4; ++i) mask[i] = byte(pos + i);
        pos += 4;
        std::string payload = c.in.substr(pos, static_cast<std::size_t>(len));
        for (std::size_t i = 0; i < payload.size(); ++i) {
            payload[i] = static_cast<char>(payload[i] ^ mask[i % 4]);
        }
        c.in.erase(0, pos + static_cast<std::size_t>(len));

        if (opcode == 0x1) {  // Text (we do not support fragmentation).
            if (on_message) on_message(payload);
        } else if (opcode == 0x8) {  // Close.
            c.out += frame(0x8, "");
            c.close_after_flush = true;
            flush(c);
            return;
        } else if (opcode == 0x9) {  // Ping.
            c.out += frame(0xA, payload);
            flush(c);
        }
    }
}

std::string WsServer::frame(std::uint8_t opcode, const std::string& payload) {
    std::string f;
    f.push_back(static_cast<char>(0x80 | opcode));
    const std::size_t n = payload.size();
    if (n < 126) {
        f.push_back(static_cast<char>(n));
    } else if (n <= 0xFFFF) {
        f.push_back(static_cast<char>(126));
        f.push_back(static_cast<char>((n >> 8) & 0xFF));
        f.push_back(static_cast<char>(n & 0xFF));
    } else {
        f.push_back(static_cast<char>(127));
        for (int i = 7; i >= 0; --i) {
            f.push_back(static_cast<char>((static_cast<std::uint64_t>(n) >>
                                           (i * 8)) & 0xFF));
        }
    }
    return f + payload;
}

bool WsServer::send_text(const std::string& message) {
    const std::string f = frame(0x1, message);
    bool any = false;
    for (Conn& c : conns_) {
        if (c.fd == -1 || !c.websocket || c.close_after_flush) continue;
        any = true;
        if (c.out.size() > kMaxOutBuffer) continue;  // Slow client: drop.
        c.out += f;
        flush(c);
    }
    return any;
}

void WsServer::flush(Conn& c) {
    while (c.fd != -1 && !c.out.empty()) {
        const ssize_t n = ::send(c.fd, c.out.data(), c.out.size(), 0);
        if (n > 0) {
            c.out.erase(0, static_cast<std::size_t>(n));
            continue;
        }
        if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        if (n == -1 && errno == EINTR) continue;
        close_conn(c);
        return;
    }
    if (c.fd != -1 && c.out.empty() && c.close_after_flush) close_conn(c);
}

void WsServer::close_conn(Conn& c) {
    if (c.fd != -1) {
        ::close(c.fd);
        c.fd = -1;
    }
    c.in.clear();
    c.out.clear();
}

} // namespace surgisafe::net
