#pragma once

// Background CPU load for the "Stress the CPU" demo.
//
// Starts one busy-looping thread per CPU, each at a LOW priority (priority 5
// on QNX, below the gateway at 50 and the safety monitor at 60). The point
// of the demo: with QNX's priority-based preemptive scheduler, the safety
// path keeps its timing while the CPUs are 100% busy, because the low-priority
// load only ever runs when nothing more important is ready.
//
// On macOS the load threads keep their default priority; the simulator's loop
// thread uses Mach real-time scheduling, the closest Mac equivalent.
//
// The load switches itself off after a time limit, so a forgotten stress test
// cannot leave the QNX VM busy.

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

namespace surgisafe {

class CpuStress {
public:
    static constexpr int kLowPriority = 5;

    CpuStress() = default;
    ~CpuStress() { stop(); }
    CpuStress(const CpuStress&) = delete;
    CpuStress& operator=(const CpuStress&) = delete;

    // Starts the load (no-op if running). Returns the number of threads.
    int start(std::uint64_t now_ns, std::uint64_t duration_ns);
    void stop();
    // Call regularly: stops the load once its time limit has passed.
    // Returns true if it stopped the load in this call.
    bool expire(std::uint64_t now_ns);

    bool running() const { return !threads_.empty(); }
    int threads() const { return static_cast<int>(threads_.size()); }
    // True if the threads really got the low priority (QNX).
    bool low_priority() const { return low_priority_; }
    // Seconds left before the automatic stop.
    double seconds_left(std::uint64_t now_ns) const;
    // Work done by the load, in millions of loop iterations per second,
    // measured over the interval since the previous call.
    double work_rate(std::uint64_t now_ns);

private:
    std::vector<std::thread> threads_;
    std::shared_ptr<std::atomic<bool>> run_;
    std::shared_ptr<std::atomic<std::uint64_t>> work_;
    std::uint64_t until_ns_ = 0;
    std::uint64_t rate_t_ns_ = 0;
    std::uint64_t rate_work_ = 0;
    double rate_ = 0.0;
    bool low_priority_ = false;
};

} // namespace surgisafe
