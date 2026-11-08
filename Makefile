# Host targets (macOS/Linux, no QNX needed):
#   make test      build and run the safety-core unit tests with ASan/UBSan
#   make campaign  run every fault scenario against the core (simulated time)
#   make demo      run the dashboard simulator, then open http://localhost:8080
#
# QNX targets (run inside a QNX SDP 8.0 environment):
#   make qnx       cross-compile controller and safety_monitor

BUILD := build

# ---- Host ----------------------------------------------------------------
CXX ?= c++
HOST_CXXFLAGS := -std=c++17 -Wall -Wextra -Wpedantic -Werror -g -O1 \
                 -fsanitize=address,undefined -fno-omit-frame-pointer \
                 -fno-sanitize-recover=all -I.

CORE_SRC := safety_core.cpp
CORE_HDR := safety_core.hpp motion_shaper.hpp
SCEN_SRC := scenarios.cpp
SCEN_HDR := scenarios.hpp

LIVE_SRC := live_controller.cpp
LIVE_HDR := live_controller.hpp
NET_SRC  := net/sha1.cpp net/ws_server.cpp
NET_HDR  := net/sha1.hpp net/ws_server.hpp

TEST_SRC := tests/safety_core_test.cpp tests/property_test.cpp tests/live_test.cpp
TEST_HDR := tests/test_framework.hpp

$(BUILD)/safety_core_test: $(TEST_SRC) $(TEST_HDR) $(CORE_SRC) $(CORE_HDR) $(LIVE_SRC) $(LIVE_HDR) net/sha1.cpp net/sha1.hpp
	@mkdir -p $(BUILD)
	$(CXX) $(HOST_CXXFLAGS) $(TEST_SRC) $(CORE_SRC) $(LIVE_SRC) net/sha1.cpp -o $@

$(BUILD)/fault_campaign: tools/fault_campaign.cpp $(SCEN_SRC) $(SCEN_HDR) $(CORE_SRC) $(CORE_HDR)
	@mkdir -p $(BUILD)
	$(CXX) $(HOST_CXXFLAGS) tools/fault_campaign.cpp $(SCEN_SRC) $(CORE_SRC) -o $@

# The simulator is built without sanitizers so its loop timing is closer
# to what it will be on QNX.
SIM_CXXFLAGS := -std=c++17 -Wall -Wextra -Wpedantic -Werror -O2 -I.

$(BUILD)/sim_server: sim/sim_server.cpp $(LIVE_SRC) $(LIVE_HDR) $(NET_SRC) $(NET_HDR) $(CORE_SRC) $(CORE_HDR)
	@mkdir -p $(BUILD)
	$(CXX) $(SIM_CXXFLAGS) sim/sim_server.cpp $(LIVE_SRC) $(NET_SRC) $(CORE_SRC) -o $@

PORT ?= 8080
.PHONY: demo sim
sim: $(BUILD)/sim_server
demo: $(BUILD)/sim_server
	./$(BUILD)/sim_server --port $(PORT) --dir dashboard

.PHONY: test
test: $(BUILD)/safety_core_test
	./$(BUILD)/safety_core_test

TRIALS ?= 200
.PHONY: campaign
campaign: $(BUILD)/fault_campaign
	./$(BUILD)/fault_campaign --trials $(TRIALS) --csv $(BUILD)/campaign.csv

# ---- QNX -----------------------------------------------------------------
QCC ?= q++
QNX_VARIANT ?= gcc_ntox86_64
QNX_CXXFLAGS := -V$(QNX_VARIANT) -std=c++17 -O2 -Wall -Wextra -pthread -I.

$(BUILD)/qnx/safety_monitor: safety_monitor.cpp $(CORE_SRC) $(CORE_HDR) protocol.hpp
	@mkdir -p $(BUILD)/qnx
	$(QCC) $(QNX_CXXFLAGS) safety_monitor.cpp $(CORE_SRC) -o $@

$(BUILD)/qnx/controller: controller.cpp $(SCEN_SRC) $(SCEN_HDR) $(CORE_SRC) $(CORE_HDR) protocol.hpp
	@mkdir -p $(BUILD)/qnx
	$(QCC) $(QNX_CXXFLAGS) controller.cpp $(SCEN_SRC) $(CORE_SRC) -o $@

.PHONY: qnx
qnx: $(BUILD)/qnx/safety_monitor $(BUILD)/qnx/controller

.PHONY: clean
clean:
	rm -rf $(BUILD)
