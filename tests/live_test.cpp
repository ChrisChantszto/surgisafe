// Tests for the live demo pieces: the WebSocket handshake helpers and the
// interactive LiveController working against the real SafetyCore.

#include "live_controller.hpp"
#include "net/sha1.hpp"
#include "safety_core.hpp"
#include "test_framework.hpp"

#include <cmath>
#include <limits>
#include <string>

using namespace surgisafe;

namespace {

std::string hex(const std::array<std::uint8_t, 20>& d) {
    static const char* k = "0123456789abcdef";
    std::string s;
    for (auto b : d) {
        s.push_back(k[b >> 4]);
        s.push_back(k[b & 15]);
    }
    return s;
}

// --- Handshake helpers (published test vectors) ----------------------------

TEST(net_sha1_vectors) {
    CHECK(hex(net::sha1("")) == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    CHECK(hex(net::sha1("abc")) == "a9993e364706816aba3e25717850c26c9cd0d89d");
    CHECK(hex(net::sha1(
              "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")) ==
          "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
    CHECK(hex(net::sha1(std::string(1000, 'a'))) ==
          "291e9a6c66994949b57ba5e650361e98fc36b1ba");
}

TEST(net_base64_padding) {
    const auto b64 = [](const std::string& s) {
        return net::base64(reinterpret_cast<const std::uint8_t*>(s.data()),
                           s.size());
    };
    CHECK(b64("") == "");
    CHECK(b64("f") == "Zg==");
    CHECK(b64("fo") == "Zm8=");
    CHECK(b64("foo") == "Zm9v");
    CHECK(b64("foobar") == "Zm9vYmFy");
}

TEST(net_websocket_accept_rfc6455_example) {
    CHECK(net::websocket_accept("dGhlIHNhbXBsZSBub25jZQ==") ==
          "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
}

// --- LiveController + SafetyCore -------------------------------------------

constexpr std::uint64_t kPeriod = 10ULL * 1000 * 1000;

struct Rig {
    Envelope env = default_envelope();
    SafetyCore core{env, {1.0, 1.0}};
    LiveController ctl{env, {1.0, 1.0}};
    std::uint64_t now = 1'000'000'000ULL;

    // Runs `cycles` control cycles, checking the watchdog every 1 ms like
    // the simulator does. Returns false once the core has stopped.
    bool run(int cycles) {
        for (int i = 0; i < cycles; ++i) {
            for (int ms = 0; ms < 10; ++ms) {
                now += kPeriod / 10;
                core.on_tick(now);
            }
            if (core.stopped()) return false;
            std::uint32_t cycle = 0;
            Point p{};
            if (ctl.make_request(now, cycle, p)) {
                if (!core.on_request(cycle, p, now).permitted) return false;
            }
        }
        return !core.stopped();
    }
};

TEST(live_follows_fresh_targets_without_stopping) {
    Rig r;
    for (int i = 0; i < 300; ++i) {
        r.ctl.set_target({3.0, 1.0}, r.now);
        CHECK(r.run(1));
    }
    CHECK(std::fabs(r.core.position().x - 3.0) < 1e-3);
    CHECK(!r.ctl.holding());
}

TEST(live_stale_target_holds_without_stopping) {
    Rig r;
    r.ctl.set_target({7.0, 1.0}, r.now);
    CHECK(r.run(20));                // Gets moving toward the target...
    CHECK(r.run(200));               // ...then the target goes stale.
    CHECK(r.ctl.holding());
    CHECK(!r.core.stopped());        // Holding is safe, not a fault.
    CHECK(std::hypot(r.core.velocity().x, r.core.velocity().y) < 1e-9);
    CHECK(r.core.position().x < 7.0);  // Braked before reaching it.
}

TEST(live_targets_are_clamped_to_workspace) {
    Rig r;
    for (int i = 0; i < 400; ++i) {
        r.ctl.set_target({50.0, -50.0}, r.now);
        CHECK(r.run(1));
    }
    CHECK(std::fabs(r.core.position().x - r.env.max_x) < 1e-3);
    CHECK(std::fabs(r.core.position().y - r.env.min_y) < 1e-3);
}

TEST(live_non_finite_target_is_ignored) {
    Rig r;
    r.ctl.set_target({std::numeric_limits<double>::quiet_NaN(), 2.0}, r.now);
    CHECK(!r.ctl.has_target());
    CHECK(r.run(10));
}

TEST(live_target_inside_zone_ends_in_nogo_stop) {
    Rig r;
    bool ok = true;
    for (int i = 0; i < 500 && ok; ++i) {
        r.ctl.set_target({5.0, 5.0}, r.now);
        ok = r.run(1);
    }
    CHECK(r.core.stop_reason() == StopReason::NoGoZone);
}

TEST(live_freeze_20ms_triggers_watchdog) {
    Rig r;
    r.ctl.set_target({3.0, 3.0}, r.now);
    CHECK(r.run(10));
    r.ctl.freeze_for(20ULL * 1000 * 1000, r.now);
    r.run(5);
    CHECK(r.core.stop_reason() == StopReason::WatchdogTimeout);
}

TEST(live_short_freeze_is_tolerated) {
    Rig r;
    r.ctl.set_target({3.0, 3.0}, r.now);
    CHECK(r.run(10));
    r.ctl.freeze_for(3ULL * 1000 * 1000, r.now);
    CHECK(r.run(20));
}

TEST(live_kill_triggers_watchdog) {
    Rig r;
    CHECK(r.run(5));
    r.ctl.kill();
    r.run(5);
    CHECK(r.core.stop_reason() == StopReason::WatchdogTimeout);
}

TEST(live_rogue_commands_are_stopped) {
    Rig r;
    CHECK(r.run(5));
    r.ctl.set_rogue(true);
    r.run(50);
    CHECK(r.core.stopped());
    CHECK(r.core.stop_reason() != StopReason::WatchdogTimeout);
}

TEST(live_reset_restarts_cleanly) {
    Rig r;
    CHECK(r.run(5));  // Arm the watchdog with a few heartbeats first.
    r.ctl.kill();
    r.run(5);
    CHECK(r.core.stopped());
    const Point at = r.core.position();
    r.core = SafetyCore(r.env, at);
    r.ctl.reset(at);
    CHECK(r.ctl.mode() == ControllerMode::Running);
    r.ctl.set_target({2.0, 2.0}, r.now);
    CHECK(r.run(50));
}

} // namespace
