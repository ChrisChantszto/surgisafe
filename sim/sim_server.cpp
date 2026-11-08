// Host simulator for the SurgiSafe-RT dashboard (no QNX needed).
//
//   make demo        then open http://localhost:8080
//
// Runs the real SafetyCore and LiveController in one process with a 10 ms
// control loop, serves the dashboard, and exchanges JSON with it over a
// WebSocket. In the QNX build the same pieces are split across processes:
// the gateway (network + LiveController) talks to safety_monitor
// (SafetyCore) with MsgSend, and the watchdog is a QNX timer pulse. Here the
// watchdog is checked every millisecond of the loop instead.
//
// Timing in this simulator is macOS/Linux best-effort, not real-time.

#include "live_controller.hpp"
#include "net/ws_server.hpp"
#include "safety_core.hpp"

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

using namespace surgisafe;

namespace {

constexpr std::uint64_t kPeriodNs = 10ULL * 1000 * 1000;
volatile std::sig_atomic_t g_running = 1;

void on_signal(int) { g_running = 0; }

std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

// Minimal extraction from the flat JSON objects the dashboard sends.
// Untrusted input: anything unexpected is simply ignored.
bool json_string(const std::string& s, const char* key, std::string& out) {
    const std::string k = std::string("\"") + key + "\"";
    auto p = s.find(k);
    if (p == std::string::npos) return false;
    p = s.find(':', p + k.size());
    if (p == std::string::npos) return false;
    p = s.find('"', p);
    if (p == std::string::npos) return false;
    const auto e = s.find('"', p + 1);
    if (e == std::string::npos || e - p - 1 > 32) return false;
    out = s.substr(p + 1, e - p - 1);
    return true;
}

bool json_number(const std::string& s, const char* key, double& out) {
    const std::string k = std::string("\"") + key + "\"";
    auto p = s.find(k);
    if (p == std::string::npos) return false;
    p = s.find(':', p + k.size());
    if (p == std::string::npos) return false;
    const char* begin = s.c_str() + p + 1;
    char* end = nullptr;
    out = std::strtod(begin, &end);
    return end != begin;
}

bool json_bool(const std::string& s, const char* key, bool& out) {
    const std::string k = std::string("\"") + key + "\"";
    auto p = s.find(k);
    if (p == std::string::npos) return false;
    p = s.find(':', p + k.size());
    if (p == std::string::npos) return false;
    p = s.find_first_not_of(" \t", p + 1);
    if (p == std::string::npos) return false;
    if (s.compare(p, 4, "true") == 0) out = true;
    else if (s.compare(p, 5, "false") == 0) out = false;
    else return false;
    return true;
}

std::string envelope_json(const Envelope& env) {
    std::string zones;
    for (std::size_t i = 0; i < env.zone_count; ++i) {
        char z[96];
        std::snprintf(z, sizeof z, "%s{\"x\":%.4f,\"y\":%.4f,\"r\":%.4f}",
                      i ? "," : "", env.zones[i].center.x,
                      env.zones[i].center.y, env.zones[i].radius);
        zones += z;
    }
    char buf[512];
    std::snprintf(buf, sizeof buf,
                  "{\"type\":\"envelope\",\"min_x\":%.4f,\"max_x\":%.4f,"
                  "\"min_y\":%.4f,\"max_y\":%.4f,\"max_step\":%.4f,"
                  "\"max_step_change\":%.4f,\"timeout_ms\":%.3f,"
                  "\"period_ms\":%.3f,\"zones\":[",
                  env.min_x, env.max_x, env.min_y, env.max_y, env.max_step,
                  env.max_step_change, env.watchdog_timeout_ns / 1e6,
                  kPeriodNs / 1e6);
    return std::string(buf) + zones + "]}";
}

std::string event_json(const char* level, const std::string& text) {
    std::string escaped;
    for (char ch : text) {
        if (ch == '"' || ch == '\\') escaped.push_back('\\');
        escaped.push_back(ch);
    }
    return std::string("{\"type\":\"event\",\"level\":\"") + level +
           "\",\"text\":\"" + escaped + "\"}";
}

struct Sim {
    Envelope env = default_envelope();
    Point start{1.0, 1.0};
    std::unique_ptr<SafetyCore> core;
    std::unique_ptr<LiveController> ctl;
    std::uint64_t last_accept_ns = 0;
    std::uint64_t accepted = 0;
    std::uint64_t stops = 0;
    double last_silence_ms = 0.0;

    Sim() { reset(start); }

    void reset(Point at) {
        core = std::make_unique<SafetyCore>(env, at);
        if (!ctl) ctl = std::make_unique<LiveController>(env, at);
        ctl->reset(at);
        last_accept_ns = 0;
    }
};

} // namespace

int main(int argc, char** argv) {
    int port = 8080;
    std::string static_dir = "dashboard";
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--dir") == 0 && i + 1 < argc) {
            static_dir = argv[++i];
        } else {
            std::fprintf(stderr, "Usage: %s [--port N] [--dir DASHBOARD_DIR]\n",
                         argv[0]);
            return 2;
        }
    }
    if (port <= 0 || port > 65535) return 2;

    std::signal(SIGPIPE, SIG_IGN);
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    net::WsServer server;
    // Loopback only: the simulator is not meant to be reachable from the
    // network.
    if (!server.listen("127.0.0.1", static_cast<std::uint16_t>(port),
                       static_dir)) {
        return 1;
    }
    std::printf("SurgiSafe-RT simulator: open http://localhost:%d\n", port);
    std::fflush(stdout);

    Sim sim;
    auto notify = [&](const char* level, const std::string& text) {
        std::printf("[%s] %s\n", level, text.c_str());
        std::fflush(stdout);
        server.send_text(event_json(level, text));
    };
    auto report_stop = [&](std::uint64_t at_ns, std::uint32_t cycle) {
        ++sim.stops;
        char text[160];
        if (sim.core->stop_reason() == StopReason::WatchdogTimeout &&
            sim.last_accept_ns) {
            sim.last_silence_ms = (at_ns - sim.last_accept_ns) / 1e6;
            std::snprintf(text, sizeof text,
                          "SAFE_STOP: watchdog timeout, %.2f ms after the "
                          "last heartbeat",
                          sim.last_silence_ms);
        } else {
            std::snprintf(text, sizeof text, "SAFE_STOP: %s (cycle %u)",
                          to_string(sim.core->stop_reason()), cycle);
        }
        notify("stop", text);
    };

    server.on_open = [&] {
        server.send_text(envelope_json(sim.env));
        notify("info", "Dashboard connected to simulator");
    };
    server.on_message = [&](const std::string& msg) {
        std::string cmd;
        if (!json_string(msg, "cmd", cmd)) return;
        const std::uint64_t now = now_ns();
        if (cmd == "target") {
            double x = 0, y = 0;
            if (json_number(msg, "x", x) && json_number(msg, "y", y)) {
                sim.ctl->set_target(Point{x, y}, now);
            }
        } else if (cmd == "reset") {
            if (sim.core->stopped()) {
                sim.reset(sim.core->position());
                notify("info", "Operator reset: motion re-enabled");
            }
        } else if (cmd == "freeze") {
            double ms = 20;
            json_number(msg, "ms", ms);
            if (ms > 0 && ms <= 1000) {
                sim.ctl->freeze_for(static_cast<std::uint64_t>(ms * 1e6), now);
                notify("fault", "Injected controller freeze: " +
                                    std::to_string(static_cast<int>(ms)) +
                                    " ms");
            }
        } else if (cmd == "kill") {
            sim.ctl->kill();
            notify("fault", "Injected controller crash");
        } else if (cmd == "rogue") {
            bool on = true;
            json_bool(msg, "on", on);
            sim.ctl->set_rogue(on);
            notify("fault", on ? "Controller compromised: rogue commands"
                               : "Rogue commands off");
        }
    };

    std::uint64_t next_release = now_ns() + kPeriodNs;
    std::uint32_t last_cycle = 0;
    while (g_running) {
        std::uint64_t now = now_ns();
        const int wait_ms = next_release > now ? 1 : 0;
        server.poll(wait_ms);
        now = now_ns();

        // Watchdog: in the QNX monitor this is a timer pulse at the
        // deadline; here we check every loop iteration (about 1 ms).
        if (!sim.core->stopped()) {
            sim.core->on_tick(now);
            if (sim.core->stopped()) report_stop(now, last_cycle);
        }

        if (now < next_release) continue;
        next_release += kPeriodNs;
        if (next_release + 100 * kPeriodNs < now) {
            next_release = now + kPeriodNs;  // Resync after a long pause.
        }

        std::uint32_t cycle = 0;
        Point proposal{};
        // A stopped monitor gets no more requests: the controller in the
        // QNX build exits on SAFE_STOP; here it simply waits for a reset.
        if (!sim.core->stopped() &&
            sim.ctl->make_request(now, cycle, proposal)) {
            last_cycle = cycle;
            const Decision d = sim.core->on_request(cycle, proposal, now);
            if (d.permitted) {
                ++sim.accepted;
                sim.last_accept_ns = now;
            } else {
                report_stop(now, cycle);
            }
        }

        const Point p = sim.core->position();
        const Point t = sim.ctl->target();
        char state[512];
        std::snprintf(
            state, sizeof state,
            "{\"type\":\"state\",\"cycle\":%u,\"x\":%.5f,\"y\":%.5f,"
            "\"vx\":%.5f,\"vy\":%.5f,\"tx\":%.5f,\"ty\":%.5f,"
            "\"has_target\":%s,\"stopped\":%s,\"reason\":\"%s\",\"code\":%d,"
            "\"holding\":%s,\"controller\":\"%s\",\"accepted\":%llu,"
            "\"stops\":%llu,\"silence_ms\":%.3f}",
            last_cycle, p.x, p.y, sim.core->velocity().x,
            sim.core->velocity().y, t.x, t.y,
            sim.ctl->has_target() ? "true" : "false",
            sim.core->stopped() ? "true" : "false",
            to_string(sim.core->stop_reason()),
            static_cast<int>(sim.core->stop_reason()),
            sim.ctl->holding() ? "true" : "false",
            to_string(sim.ctl->mode()),
            static_cast<unsigned long long>(sim.accepted),
            static_cast<unsigned long long>(sim.stops), sim.last_silence_ms);
        server.send_text(state);
    }
    std::printf("Simulator stopped\n");
    return 0;
}
