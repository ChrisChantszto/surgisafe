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
#include "net/json_util.hpp"
#include "net/ws_server.hpp"
#include "safety_core.hpp"

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#endif

using namespace surgisafe;
using namespace surgisafe::net;

namespace {

constexpr std::uint64_t kPeriodNs = 10ULL * 1000 * 1000;
volatile std::sig_atomic_t g_running = 1;

// macOS coalesces timers of ordinary processes to save power, which can
// delay a wake-up by several milliseconds and trip the 15 ms watchdog.
// Ask for Mach "time-constraint" (real-time) scheduling for the loop thread,
// as audio software does. No privileges needed. Linux: no-op here.
bool request_realtime_scheduling() {
#ifdef __APPLE__
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    const auto abs = [&](double ms) {
        return static_cast<std::uint32_t>(ms * 1e6 * tb.denom / tb.numer);
    };
    thread_time_constraint_policy_data_t policy;
    policy.period = abs(1.0);        // The loop runs about every 1 ms...
    policy.computation = abs(0.3);   // ...needing up to 0.3 ms of CPU...
    policy.constraint = abs(1.0);    // ...within each 1 ms window.
    policy.preemptible = 1;
    return thread_policy_set(mach_thread_self(),
                             THREAD_TIME_CONSTRAINT_POLICY,
                             reinterpret_cast<thread_policy_t>(&policy),
                             THREAD_TIME_CONSTRAINT_POLICY_COUNT) ==
           KERN_SUCCESS;
#else
    return false;
#endif
}

void on_signal(int) { g_running = 0; }

std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
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
    const bool rt = request_realtime_scheduling();
    std::printf("SurgiSafe-RT simulator: open http://localhost:%d%s\n", port,
                rt ? " (real-time thread scheduling on)" : "");
    std::fflush(stdout);

    Sim sim;
    std::uint32_t last_cycle = 0;  // Last cycle sent; used in stop reports.
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
        } else if (sim.core->stop_reason() == StopReason::ContactForce) {
            std::snprintf(text, sizeof text,
                          "SAFE_STOP: contact force %.2f N (limit %.1f N)",
                          sim.core->last_force(),
                          sim.env.max_contact_force_n);
        } else {
            std::snprintf(text, sizeof text, "SAFE_STOP: %s (cycle %u)",
                          to_string(sim.core->stop_reason()), cycle);
        }
        notify("stop", text);
    };

    server.on_open = [&] {
        server.send_text(envelope_json(sim.env, kPeriodNs, "mac-sim"));
        notify("info", "Client connected to simulator");
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
        } else if (cmd == "force") {
            // Contact-force sensor reading from the physics simulation
            // (MuJoCo bridge). On a real system the sensor would be wired to
            // the monitor directly, not through the planner's channel.
            double n = 0;
            if (json_number(msg, "n", n)) {
                const bool was_stopped = sim.core->stopped();
                sim.core->on_force(n);
                if (!was_stopped && sim.core->stopped()) {
                    report_stop(now, last_cycle);
                }
            }
        } else if (cmd == "rogue") {
            bool on = true;
            json_bool(msg, "on", on);
            sim.ctl->set_rogue(on);
            notify("fault", on ? "Controller compromised: rogue commands"
                               : "Rogue commands off");
        }
    };

    std::uint64_t next_release = now_ns() + kPeriodNs;
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

        Telemetry t;
        t.cycle = last_cycle;
        t.position = sim.core->position();
        t.velocity = sim.core->velocity();
        t.target = sim.ctl->target();
        t.has_target = sim.ctl->has_target();
        t.stopped = sim.core->stopped();
        t.holding = sim.ctl->holding();
        t.reason = sim.core->stop_reason();
        t.controller = sim.ctl->mode();
        t.accepted = sim.accepted;
        t.stops = sim.stops;
        t.silence_ms = sim.last_silence_ms;
        t.force = sim.core->last_force();
        server.send_text(state_json(t));
    }
    std::printf("Simulator stopped\n");
    return 0;
}
