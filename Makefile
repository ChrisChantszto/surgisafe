# Host targets (macOS/Linux, no QNX needed):
#   make test      build and run the safety-core unit tests with ASan/UBSan
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
CORE_HDR := safety_core.hpp

$(BUILD)/safety_core_test: tests/safety_core_test.cpp $(CORE_SRC) $(CORE_HDR)
	@mkdir -p $(BUILD)
	$(CXX) $(HOST_CXXFLAGS) tests/safety_core_test.cpp $(CORE_SRC) -o $@

.PHONY: test
test: $(BUILD)/safety_core_test
	./$(BUILD)/safety_core_test

# ---- QNX -----------------------------------------------------------------
QCC ?= q++
QNX_VARIANT ?= gcc_ntox86_64
QNX_CXXFLAGS := -V$(QNX_VARIANT) -std=c++17 -O2 -Wall -Wextra -pthread

$(BUILD)/qnx/safety_monitor: safety_monitor.cpp $(CORE_SRC) $(CORE_HDR) protocol.hpp
	@mkdir -p $(BUILD)/qnx
	$(QCC) $(QNX_CXXFLAGS) safety_monitor.cpp $(CORE_SRC) -o $@

$(BUILD)/qnx/controller: controller.cpp $(CORE_SRC) $(CORE_HDR) protocol.hpp
	@mkdir -p $(BUILD)/qnx
	$(QCC) $(QNX_CXXFLAGS) controller.cpp $(CORE_SRC) -o $@

.PHONY: qnx
qnx: $(BUILD)/qnx/safety_monitor $(BUILD)/qnx/controller

.PHONY: clean
clean:
	rm -rf $(BUILD)
