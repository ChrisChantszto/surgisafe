# Host targets (macOS/Linux, no QNX needed):
#   make test      build and run the safety-core unit tests with ASan/UBSan
#   make campaign  run every fault scenario against the core (simulated time)
#   make demo      run the dashboard simulator, then open http://localhost:8080
#   make mujoco    3D robot arm (MuJoCo) driven by the simulator; run beside
#                  `make demo` (first time: make mujoco-setup)
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
STRESS_SRC := cpu_stress.cpp
STRESS_HDR := cpu_stress.hpp
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

$(BUILD)/sim_server: sim/sim_server.cpp $(LIVE_SRC) $(LIVE_HDR) $(NET_SRC) $(NET_HDR) net/json_util.hpp $(STRESS_SRC) $(STRESS_HDR) $(CORE_SRC) $(CORE_HDR)
	@mkdir -p $(BUILD)
	$(CXX) $(SIM_CXXFLAGS) sim/sim_server.cpp $(LIVE_SRC) $(NET_SRC) $(STRESS_SRC) $(CORE_SRC) -pthread -o $@

PORT ?= 8080
.PHONY: demo sim
sim: $(BUILD)/sim_server
demo: $(BUILD)/sim_server
	./$(BUILD)/sim_server --port $(PORT) --dir dashboard

# ---- MuJoCo robot-arm simulation (Python) ---------------------------------
MUJOCO_VENV := .venv-mujoco
.PHONY: mujoco-setup mujoco mujoco-headless
mujoco-setup:
	python3 -m venv $(MUJOCO_VENV)
	$(MUJOCO_VENV)/bin/pip install -r sim/mujoco/requirements.txt
# mjpython is required on macOS for MuJoCo's interactive viewer.
mujoco:
	$(MUJOCO_VENV)/bin/mjpython sim/mujoco/bridge.py --url ws://127.0.0.1:$(PORT)/ws
mujoco-headless:
	$(MUJOCO_VENV)/bin/python sim/mujoco/bridge.py --url ws://127.0.0.1:$(PORT)/ws --headless

.PHONY: test
test: $(BUILD)/safety_core_test
	./$(BUILD)/safety_core_test

TRIALS ?= 200
.PHONY: campaign
campaign: $(BUILD)/fault_campaign
	./$(BUILD)/fault_campaign --trials $(TRIALS) --profile hardware --csv $(BUILD)/campaign.csv
	./$(BUILD)/fault_campaign --trials $(TRIALS) --profile qemu --csv $(BUILD)/campaign-qemu.csv

# ---- QNX -----------------------------------------------------------------
QCC ?= q++
QNX_VARIANT ?= gcc_ntox86_64
# _QNX_SOURCE exposes POSIX timers and QNX extensions (SIGEV_PULSE_INIT,
# ...) that strict ISO mode hides in the QNX headers.
QNX_CXXFLAGS := -V$(QNX_VARIANT) -std=gnu++17 -D_QNX_SOURCE -O2 -Wall -Wextra -Werror -I.

$(BUILD)/qnx/safety_monitor: safety_monitor.cpp $(CORE_SRC) $(CORE_HDR) protocol.hpp
	@mkdir -p $(BUILD)/qnx
	$(QCC) $(QNX_CXXFLAGS) safety_monitor.cpp $(CORE_SRC) -o $@

$(BUILD)/qnx/controller: controller.cpp $(SCEN_SRC) $(SCEN_HDR) $(CORE_SRC) $(CORE_HDR) protocol.hpp
	@mkdir -p $(BUILD)/qnx
	$(QCC) $(QNX_CXXFLAGS) controller.cpp $(SCEN_SRC) $(CORE_SRC) -o $@

# The host unit/property tests, built for QNX so they can run on the target.
$(BUILD)/qnx/safety_core_test: $(TEST_SRC) $(TEST_HDR) $(CORE_SRC) $(CORE_HDR) $(LIVE_SRC) $(LIVE_HDR) net/sha1.cpp net/sha1.hpp
	@mkdir -p $(BUILD)/qnx
	$(QCC) $(QNX_CXXFLAGS) $(TEST_SRC) $(CORE_SRC) $(LIVE_SRC) net/sha1.cpp -o $@

# Network gateway: serves the dashboard and forwards to safety_monitor.
# QNX keeps socket functions in libsocket.
$(BUILD)/qnx/qnx_gateway: qnx_gateway.cpp $(LIVE_SRC) $(LIVE_HDR) $(NET_SRC) $(NET_HDR) net/json_util.hpp $(STRESS_SRC) $(STRESS_HDR) $(CORE_SRC) $(CORE_HDR) protocol.hpp
	@mkdir -p $(BUILD)/qnx
	$(QCC) $(QNX_CXXFLAGS) qnx_gateway.cpp $(LIVE_SRC) $(NET_SRC) $(STRESS_SRC) $(CORE_SRC) -lsocket -o $@

.PHONY: qnx
qnx: $(BUILD)/qnx/safety_monitor $(BUILD)/qnx/controller $(BUILD)/qnx/safety_core_test $(BUILD)/qnx/qnx_gateway

# ---- QNX workflow, run from the Mac ---------------------------------------
# Needs the qnx-build container (qnx/docker/setup.sh) with the SDP in
# ~/qnx800, and QEMU (brew install qemu). See qnx/README.md.
QNX_TARGET_VARIANT ?= gcc_ntoaarch64le
REPO_IN_CONTAINER := /home/$(shell id -un)$(subst $(HOME),,$(CURDIR))
REPEATS ?= 3
PROFILE ?= qemu

.PHONY: qnx-build qnx-image qnx-boot qnx-deploy qnx-test qnx-scenarios qnx-report qnx-live
qnx-build:
	docker exec qnx-build bash -c 'source ~/qnx800/qnxsdp-env.sh > /dev/null && cd "$(REPO_IN_CONTAINER)" && make qnx QNX_VARIANT=$(QNX_TARGET_VARIANT)'
qnx-image:
	qnx/vm/image.sh
qnx-boot:
	qnx/vm/boot.sh
qnx-deploy: qnx-build
	qnx/vm/target.sh deploy
qnx-test: qnx-deploy
	qnx/vm/target.sh ssh /data/surgisafe/safety_core_test
qnx-scenarios: qnx-deploy
	qnx/vm/target.sh ssh /data/surgisafe/scenarios_on_target.sh $(REPEATS) $(PROFILE)

# Live demo with QNX in charge: safety_monitor + gateway on QNX. Then open
# http://localhost:8088 and run `make mujoco PORT=8088` in another terminal.
qnx-live: qnx-deploy
	qnx/vm/target.sh ssh -t 'cd /data/surgisafe && (on -p 60 ./safety_monitor --profile $(PROFILE) &) && sleep 1 && on -p 50 ./qnx_gateway --profile $(PROFILE) --dir dashboard; slay -f safety_monitor'

# Fetch the last qnx-scenarios results and build build/qnx-report.html.
qnx-report:
	qnx/vm/target.sh fetch
	python3 tools/qnx_report.py
	@echo "Open it with: open build/qnx-report.html"

.PHONY: clean
clean:
	rm -rf $(BUILD)
