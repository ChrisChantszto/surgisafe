// QNX network gateway: puts the real QNX safety monitor in charge of the
// live demo (browser dashboard + MuJoCo arm on the Mac).
//
//   on -p 60 ./safety_monitor --profile qemu &
//   on -p 50 ./qnx_gateway --profile qemu --dir dashboard
//
// It serves the dashboard and speaks the same WebSocket/JSON protocol as the
// Mac simulator (net/json_util.hpp), but it does NOT decide anything itself:
//   - every 10 ms it proposes the next step toward the planner's target and
//     sends it to safety_monitor with MsgSend; the monitor approves or stops;
//   - force readings from the MuJoCo arm go to the monitor (ForceReport);
//   - operator reset goes to the monitor (ResetRequest);
//   - when it is not sending moves (stopped, frozen, crashed) it asks the
//     monitor for its status each cycle, so the monitor's own watchdog
//     (a QNX timer pulse) is what detects a silent controller.
// "Stress the CPU" starts low-priority busy threads (cpu_stress.hpp); every
// cycle reports how long the monitor took to decide and how late the cycle
// started, so the dashboard can show that the load does not disturb them.
// Telemetry to the Mac reports the monitor's decisions.
//
// The gateway is untrusted from the monitor's point of view, exactly like
// the scenario controller.

#include "cpu_stress.hpp"
#include "live_controller.hpp"
#include "net/json_util.hpp"
#include "net/ws_server.hpp"
#include "protocol.hpp"
#include "safety_core.hpp"

#include <sys/dispatch.h>
#include <sys/neutrino.h>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

using namespace surgisafe;
using namespace surgisafe::net;

namespace {

volatile std::sig_atomic_t g_running = 1;
void on_signal(int) { g_running = 0; }

std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

// What the gateway knows about the monitor's state (from its replies).
struct MonitorView {
    bool stopped = false;
    StopReason reason = StopReason::None;
    Point position{0, 0};
    Point velocity{0, 0};
    double force = 0.0;
};

class MonitorLink {
public:
    explicit MonitorLink(int coid) : coid_(coid) {}

    bool move(std::uint32_t cycle, Point p, MonitorView& v) {
        const MoveRequest req{kMoveRequest, 0, cycle, p.x, p.y};
        MoveReply rep{};
        if (MsgSend(coid_, &req, sizeof req, &rep, sizeof rep) == -1) {
            std::perror("MsgSend(move)");
            return false;
        }
        const Point before = v.position;
        v.position = Point{rep.x, rep.y};
        v.velocity = rep.permitted
                         ? Point{rep.x - before.x, rep.y - before.y}
                         : Point{0, 0};
        v.stopped = !rep.permitted;
        v.reason = static_cast<StopReason>(rep.reason);
        return true;
    }

    bool simple(std::uint16_t type, MonitorView& v) {
        const SimpleRequest req{type, 0, 0};
        return reply(&req, sizeof req, v);
    }

    bool force(double newtons, MonitorView& v) {
        const ForceReport req{kForceReport, 0, 0, newtons};
        return reply(&req, sizeof req, v);
    }

private:
    bool reply(const void* req, std::size_t len, MonitorView& v) {
        MonitorStatus st{};
        if (MsgSend(coid_, req, len, &st, sizeof st) == -1) {
            std::perror("MsgSend(status)");
            return false;
        }
        v.stopped = st.stopped != 0;
        v.reason = static_cast<StopReason>(st.reason);
        v.position = Point{st.x, st.y};
        v.force = st.last_force;
        if (v.stopped) v.velocity = Point{0, 0};
        return true;
    }

    int coid_;
};

void usage(const char* argv0) {
    std::fprintf(stderr,
                 "Usage: %s [--profile hardware|qemu] [--port N] [--dir DASHBOARD]\n",
                 argv0);
}

} // namespace

int main(int argc, char** argv) {
    const TimingProfile* profile = &hardware_profile();
    int port = 8080;
    std::string dir = "dashboard";
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--profile" && i + 1 < argc) {
            profile = find_timing_profile(argv[++i]);
            if (!profile) {
                usage(argv[0]);
                return 2;
            }
        } else if (a == "--port" && i + 1 < argc) {
            port = std::atoi(argv[++i]);
        } else if (a == "--dir" && i + 1 < argc) {
            dir = argv[++i];
        } else {
            usage(argv[0]);
            return 2;
        }
    }
    if (port <= 0 || port > 65535) return 2;
    std::signal(SIGPIPE, SIG_IGN);
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    // Connect to the monitor (it may still be starting).
    int coid = -1;
    for (int tries = 0; tries < 50 && coid == -1; ++tries) {
        coid = name_open(kServiceName, 0);
        if (coid == -1) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (coid == -1) {
        std::perror("name_open (start safety_monitor first)");
        return 1;
    }
    MonitorLink monitor(coid);
    MonitorView view;
    if (!monitor.simple(kStatusQuery, view)) return 1;

    const Envelope env = default_envelope(*profile);
    const std::uint64_t period = profile->period_ns;
    LiveController ctl(env, view.position);

    WsServer server;
    // All interfaces: the Mac reaches it through QEMU's port forward.
    if (!server.listen("0.0.0.0", static_cast<std::uint16_t>(port), dir)) {
        return 1;
    }
    std::printf("SurgiSafe-RT QNX gateway on port %d (profile %s, deadline %.0f ms); "
                "decisions by safety_monitor\n",
                port, profile->name, profile->deadline_ns / 1e6);
    std::fflush(stdout);

    std::uint64_t accepted = 0, stops = 0, last_accept_ns = 0;
    double last_silence_ms = 0.0;
    std::uint32_t last_cycle = 0;
    CpuStress stress;
    constexpr std::uint64_t kStressDurationNs = 60'000'000'000ULL;

    auto notify = [&](const char* level, const std::string& text) {
        std::printf("[%s] %s\n", level, text.c_str());
        std::fflush(stdout);
        server.send_text(event_json(level, text));
    };
    // Called whenever a reply shows a newly latched stop.
    auto check_stop = [&](bool was_stopped, std::uint64_t at) {
        if (was_stopped || !view.stopped) return;
        ++stops;
        char text[160];
        if (view.reason == StopReason::WatchdogTimeout && last_accept_ns) {
            last_silence_ms = (at - last_accept_ns) / 1e6;
            std::snprintf(text, sizeof text,
                          "SAFE_STOP by QNX safety_monitor: watchdog timeout "
                          "(seen %.1f ms after the last approved move)",
                          last_silence_ms);
        } else if (view.reason == StopReason::ContactForce) {
            std::snprintf(text, sizeof text,
                          "SAFE_STOP by QNX safety_monitor: contact force "
                          "%.2f N (limit %.1f N)",
                          view.force, env.max_contact_force_n);
        } else {
            std::snprintf(text, sizeof text,
                          "SAFE_STOP by QNX safety_monitor: %s (cycle %u)",
                          to_string(view.reason), last_cycle);
        }
        notify("stop", text);
    };

    server.on_open = [&] {
        server.send_text(envelope_json(env, period, "qnx"));
        notify("info", "Client connected to the QNX gateway");
    };
    server.on_message = [&](const std::string& msg) {
        std::string cmd;
        if (!json_string(msg, "cmd", cmd)) return;
        const std::uint64_t now = now_ns();
        if (cmd == "target") {
            double x = 0, y = 0;
            if (json_number(msg, "x", x) && json_number(msg, "y", y)) {
                ctl.set_target(Point{x, y}, now);
            }
        } else if (cmd == "force") {
            double n = 0;
            if (json_number(msg, "n", n)) {
                const bool was = view.stopped;
                monitor.force(n, view);
                check_stop(was, now);
            }
        } else if (cmd == "reset") {
            if (view.stopped && monitor.simple(kResetRequest, view) &&
                !view.stopped) {
                ctl.reset(view.position);
                last_accept_ns = 0;
                notify("info", "Operator reset accepted by QNX safety_monitor");
            }
        } else if (cmd == "freeze") {
            double ms = 20;
            json_number(msg, "ms", ms);
            if (ms > 0 && ms <= 1000) {
                ctl.freeze_for(static_cast<std::uint64_t>(ms * 1e6), now);
                notify("fault", "Injected gateway freeze: " +
                                    std::to_string(static_cast<int>(ms)) + " ms");
            }
        } else if (cmd == "kill") {
            ctl.kill();
            notify("fault", "Injected gateway crash (stops sending to QNX)");
        } else if (cmd == "rogue") {
            bool on = true;
            json_bool(msg, "on", on);
            ctl.set_rogue(on);
            notify("fault", on ? "Gateway compromised: rogue commands to QNX"
                               : "Rogue commands off");
        } else if (cmd == "stress") {
            bool on = true;
            json_bool(msg, "on", on);
            if (on && !stress.running()) {
                const int n = stress.start(now_ns(), kStressDurationNs);
                notify("info", "CPU stress ON: " + std::to_string(n) +
                                   " busy threads at priority " +
                                   std::to_string(CpuStress::kLowPriority) +
                                   (stress.low_priority()
                                        ? " (safety monitor: 60, gateway: 50)"
                                        : " (could not lower their priority)") +
                                   ", for 60 s");
            } else if (!on && stress.running()) {
                stress.stop();
                notify("info", "CPU stress OFF");
            }
        }
    };

    std::uint64_t next_release = now_ns() + period;
    while (g_running) {
        server.poll(1);
        const std::uint64_t now = now_ns();
        if (now < next_release) continue;
        const double late_us = (now - next_release) / 1e3;
        next_release += period;
        if (next_release + 100 * period < now) next_release = now + period;

        const bool was = view.stopped;
        std::uint32_t cycle = 0;
        Point proposal{};
        if (stress.expire(now)) notify("info", "CPU stress OFF (60 s limit)");
        const std::uint64_t t0 = now_ns();
        if (!view.stopped && ctl.make_request(now, cycle, proposal)) {
            last_cycle = cycle;
            if (!monitor.move(cycle, proposal, view)) break;
            if (!view.stopped) {
                ++accepted;
                last_accept_ns = now;
            }
        } else if (!monitor.simple(kStatusQuery, view)) {
            break;
        }
        const double decide_us = (now_ns() - t0) / 1e3;
        check_stop(was, now);

        Telemetry t;
        t.cycle = last_cycle;
        t.position = view.position;
        t.velocity = view.velocity;
        t.target = ctl.target();
        t.has_target = ctl.has_target();
        t.stopped = view.stopped;
        t.holding = ctl.holding();
        t.reason = view.reason;
        t.controller = ctl.mode();
        t.accepted = accepted;
        t.stops = stops;
        t.silence_ms = last_silence_ms;
        t.force = view.force;
        t.decide_us = decide_us;
        t.late_us = late_us;
        t.stress = stress.running();
        t.stress_threads = stress.threads();
        t.stress_low_prio = stress.low_priority();
        t.stress_mops = stress.work_rate(now);
        t.stress_left_s = stress.seconds_left(now);
        server.send_text(state_json(t));
    }
    name_close(coid);
    std::printf("Gateway stopped\n");
    return 0;
}
