#pragma once

// JSON messages exchanged with the dashboard and the MuJoCo bridge. Shared by
// the Mac simulator (sim/sim_server.cpp) and the QNX gateway
// (qnx_gateway.cpp), so both speak exactly the same protocol.
//
// Parsing is deliberately minimal (flat objects only) and treats input as
// untrusted: anything unexpected is ignored.

#include "live_controller.hpp"
#include "safety_core.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace surgisafe::net {

inline bool json_string(const std::string& s, const char* key,
                        std::string& out) {
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

inline bool json_number(const std::string& s, const char* key, double& out) {
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

inline bool json_bool(const std::string& s, const char* key, bool& out) {
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

// `source` names who makes the safety decisions ("mac-sim" or "qnx").
inline std::string envelope_json(const Envelope& env, std::uint64_t period_ns,
                                 const char* source) {
    std::string zones;
    for (std::size_t i = 0; i < env.zone_count; ++i) {
        char z[96];
        std::snprintf(z, sizeof z, "%s{\"x\":%.4f,\"y\":%.4f,\"r\":%.4f}",
                      i ? "," : "", env.zones[i].center.x,
                      env.zones[i].center.y, env.zones[i].radius);
        zones += z;
    }
    char buf[600];
    std::snprintf(buf, sizeof buf,
                  "{\"type\":\"envelope\",\"source\":\"%s\",\"min_x\":%.4f,"
                  "\"max_x\":%.4f,\"min_y\":%.4f,\"max_y\":%.4f,"
                  "\"max_step\":%.4f,\"max_step_change\":%.4f,"
                  "\"timeout_ms\":%.3f,\"period_ms\":%.3f,\"max_force\":%.3f,"
                  "\"zones\":[",
                  source, env.min_x, env.max_x, env.min_y, env.max_y,
                  env.max_step, env.max_step_change,
                  env.watchdog_timeout_ns / 1e6, period_ns / 1e6,
                  env.max_contact_force_n);
    return std::string(buf) + zones + "]}";
}

inline std::string event_json(const char* level, const std::string& text) {
    std::string escaped;
    for (char ch : text) {
        if (ch == '"' || ch == '\\') escaped.push_back('\\');
        escaped.push_back(ch);
    }
    return std::string("{\"type\":\"event\",\"level\":\"") + level +
           "\",\"text\":\"" + escaped + "\"}";
}

// One telemetry frame, sent every control cycle.
struct Telemetry {
    std::uint32_t cycle = 0;
    Point position{0, 0};
    Point velocity{0, 0};
    Point target{0, 0};
    bool has_target = false;
    bool stopped = false;
    bool holding = false;
    StopReason reason = StopReason::None;
    ControllerMode controller = ControllerMode::Running;
    std::uint64_t accepted = 0;
    std::uint64_t stops = 0;
    double silence_ms = 0.0;
    double force = 0.0;
    // Real-time measurements for this cycle (see "Stress the CPU").
    double decide_us = 0.0;   // Time to get the safety decision.
    double late_us = 0.0;     // How late the control cycle started.
    // Background CPU load (CpuStress).
    bool stress = false;
    int stress_threads = 0;
    bool stress_low_prio = false;
    double stress_mops = 0.0;  // Work done by the load, M iterations/s.
    double stress_left_s = 0.0;
};

inline std::string state_json(const Telemetry& t) {
    char buf[1000];
    std::snprintf(
        buf, sizeof buf,
        "{\"type\":\"state\",\"cycle\":%u,\"x\":%.5f,\"y\":%.5f,"
        "\"vx\":%.5f,\"vy\":%.5f,\"tx\":%.5f,\"ty\":%.5f,"
        "\"has_target\":%s,\"stopped\":%s,\"reason\":\"%s\",\"code\":%d,"
        "\"holding\":%s,\"controller\":\"%s\",\"accepted\":%llu,"
        "\"stops\":%llu,\"silence_ms\":%.3f,\"force\":%.3f,"
        "\"decide_us\":%.1f,\"late_us\":%.1f,\"stress\":%s,"
        "\"stress_threads\":%d,\"stress_low_prio\":%s,"
        "\"stress_mops\":%.1f,\"stress_left\":%.1f}",
        t.cycle, t.position.x, t.position.y, t.velocity.x, t.velocity.y,
        t.target.x, t.target.y, t.has_target ? "true" : "false",
        t.stopped ? "true" : "false", to_string(t.reason),
        static_cast<int>(t.reason), t.holding ? "true" : "false",
        to_string(t.controller), static_cast<unsigned long long>(t.accepted),
        static_cast<unsigned long long>(t.stops), t.silence_ms, t.force,
        t.decide_us, t.late_us, t.stress ? "true" : "false", t.stress_threads,
        t.stress_low_prio ? "true" : "false", t.stress_mops, t.stress_left_s);
    return buf;
}

} // namespace surgisafe::net
