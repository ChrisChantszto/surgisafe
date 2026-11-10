#include "cpu_stress.hpp"

#include <chrono>
#include <pthread.h>
#include <sched.h>

namespace surgisafe {

namespace {

bool lower_own_priority() {
#ifdef __QNX__
    sched_param sp{};
    sp.sched_priority = CpuStress::kLowPriority;
    return pthread_setschedparam(pthread_self(), SCHED_RR, &sp) == 0;
#else
    return false;
#endif
}

void burn(std::shared_ptr<std::atomic<bool>> run,
          std::shared_ptr<std::atomic<std::uint64_t>> work) {
    volatile double x = 1.0;
    while (run->load(std::memory_order_relaxed)) {
        for (int i = 0; i < 10000; ++i) x = x * 1.0000001 + 1e-9;
        work->fetch_add(10000, std::memory_order_relaxed);
    }
}

} // namespace

int CpuStress::start(std::uint64_t now_ns, std::uint64_t duration_ns) {
    if (running()) return threads();
    unsigned n = std::thread::hardware_concurrency();
    if (n == 0) n = 2;
    if (n > 16) n = 16;
    run_ = std::make_shared<std::atomic<bool>>(true);
    work_ = std::make_shared<std::atomic<std::uint64_t>>(0);
    // Each thread lowers its own priority before it starts burning, and
    // reports whether that worked.
    auto ok = std::make_shared<std::atomic<int>>(0);
    for (unsigned i = 0; i < n; ++i) {
        threads_.emplace_back([run = run_, work = work_, ok] {
            if (lower_own_priority()) ok->fetch_add(1);
            burn(run, work);
        });
    }
    // Give the threads a moment to set their priority.
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    low_priority_ = ok->load() == static_cast<int>(n);
    until_ns_ = now_ns + duration_ns;
    rate_t_ns_ = now_ns;
    rate_work_ = 0;
    rate_ = 0.0;
    return threads();
}

void CpuStress::stop() {
    if (!running()) return;
    run_->store(false);
    for (auto& t : threads_) t.join();
    threads_.clear();
    rate_ = 0.0;
}

bool CpuStress::expire(std::uint64_t now_ns) {
    if (!running() || now_ns < until_ns_) return false;
    stop();
    return true;
}

double CpuStress::seconds_left(std::uint64_t now_ns) const {
    if (!running() || now_ns >= until_ns_) return 0.0;
    return (until_ns_ - now_ns) / 1e9;
}

double CpuStress::work_rate(std::uint64_t now_ns) {
    if (!running()) return 0.0;
    const std::uint64_t dt = now_ns - rate_t_ns_;
    if (dt >= 500'000'000ULL) {
        const std::uint64_t w = work_->load(std::memory_order_relaxed);
        rate_ = (w - rate_work_) / (dt / 1e9) / 1e6;
        rate_work_ = w;
        rate_t_ns_ = now_ns;
    }
    return rate_;
}

} // namespace surgisafe
