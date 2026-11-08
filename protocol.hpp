#pragma once

#include <cstdint>
#include <sys/iomsg.h>

namespace surgisafe {
constexpr char kServiceName[] = "surgisafe_rt";
constexpr std::uint16_t kMoveRequest = _IO_MAX + 10;
constexpr double kMaxPosition = 8.0;
constexpr double kMaxStep = 0.1;

struct MoveRequest {
    std::uint16_t type;
    std::uint16_t reserved;
    std::uint32_t cycle;
    double proposed_position;
};

struct MoveReply {
    std::int32_t permitted;
    std::int32_t reason; // 0=accepted, 1=safe stop, 2=invalid motion
    double actual_position;
};
} // namespace surgisafe
