#pragma once

// QNX IPC wire format shared by controller and safety_monitor.

#include "safety_core.hpp"

#include <cstdint>
#include <sys/iomsg.h>
#include <type_traits>

namespace surgisafe {
constexpr char kServiceName[] = "surgisafe_rt";
// Application message types must sit above the QNX I/O message range.
constexpr std::uint16_t kMoveRequest = _IO_MAX + 10;
// Used by the network gateway (qnx_gateway.cpp):
constexpr std::uint16_t kForceReport = _IO_MAX + 11;   // ForceReport
constexpr std::uint16_t kStatusQuery = _IO_MAX + 12;   // SimpleRequest
constexpr std::uint16_t kResetRequest = _IO_MAX + 13;  // SimpleRequest

struct MoveRequest {
    std::uint16_t type;
    std::uint16_t reserved;
    std::uint32_t cycle;
    double x;  // Proposed tip position, workspace units.
    double y;
};

struct MoveReply {
    std::int32_t permitted;
    std::int32_t reason;  // A StopReason value.
    double x;             // Actual tip position after the decision.
    double y;
};

// Contact-force sensor reading, forwarded to the monitor (rule SR-10).
struct ForceReport {
    std::uint16_t type;
    std::uint16_t reserved;
    std::uint32_t pad;
    double newtons;
};

// Status query or operator reset: just a type.
struct SimpleRequest {
    std::uint16_t type;
    std::uint16_t reserved;
    std::uint32_t pad;
};

// Reply to ForceReport, StatusQuery and ResetRequest.
struct MonitorStatus {
    std::int32_t stopped;
    std::int32_t reason;          // A StopReason value.
    double x;
    double y;
    double last_force;
    std::uint32_t expected_cycle;
    std::uint32_t armed;
};

static_assert(std::is_trivially_copyable_v<MoveRequest>);
static_assert(sizeof(ForceReport) == 16, "wire layout changed");
static_assert(sizeof(SimpleRequest) == 8, "wire layout changed");
static_assert(sizeof(MonitorStatus) == 40, "wire layout changed");
static_assert(std::is_trivially_copyable_v<MoveReply>);
static_assert(sizeof(MoveRequest) == 24, "wire layout changed");
static_assert(sizeof(MoveReply) == 24, "wire layout changed");
} // namespace surgisafe
