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

struct MoveRequest {
    std::uint16_t type;
    std::uint16_t reserved;
    std::uint32_t cycle;
    double proposed_position;
};

struct MoveReply {
    std::int32_t permitted;
    std::int32_t reason;  // A StopReason value.
    double actual_position;
};

static_assert(std::is_trivially_copyable_v<MoveRequest>);
static_assert(std::is_trivially_copyable_v<MoveReply>);
static_assert(sizeof(MoveRequest) == 16, "wire layout changed");
static_assert(sizeof(MoveReply) == 16, "wire layout changed");
} // namespace surgisafe
