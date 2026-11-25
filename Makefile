# Host build for the A2UI core and LVGL adapter, plus the conformance harness.
#
#   make            build the libraries and test binaries
#   make test       run unit tests and the conformance suites
#   make conformance SUITE=data_model   run one suite (substring match)
#   make lvgl-smoke  build LVGL (third_party/lvgl) and run the LVGL adapter tests
#   make adapter-conformance   run the framework adapter suites against a2ui_lvgl
#   make renders     render catalogs/basic/v1/examples and test/lvgl/renders to build/renders/*.png
#   make simulator   build the SDL desktop simulator (needs SDL2) into build/sim/
#   make e2e         run the WebSocket transport against examples/agent_surface/sample_agent.py
#   make heap-report  live heap of the basic catalog and a card (no sanitizers)
#
# The LVGL targets are part of `all` and `test` when the third_party/lvgl
# submodule is checked out; the core library and harness never depend on it.
#
# The libraries build with -fno-exceptions -fno-rtti, as they do on device,
# so host builds catch code that would not compile for ESP-IDF defaults.

CXX ?= g++
CC ?= gcc
UV ?= $(shell command -v uv 2>/dev/null || echo $(HOME)/.local/bin/uv)
BUILD := build/host
UPSTREAM := third_party/a2ui

SAN ?= -fsanitize=address,undefined -fno-sanitize=vptr -fno-omit-frame-pointer
OPT ?= -O1 -g
# Clang rejects GCC's leveled format checks and -Wmaybe-uninitialized.
IS_CLANG := $(shell $(CXX) --version 2>/dev/null | grep -qi clang && echo 1)
GCC_WARN := $(if $(IS_CLANG),,-Wformat-truncation=2 -Wformat-overflow=2)
GCC_TEST_WARN := $(if $(IS_CLANG),,-Wno-maybe-uninitialized)
WARN := -Wall -Wextra -Wno-unused-parameter -Wformat=2 $(GCC_WARN)

LIB_CXXFLAGS := -std=c++17 $(OPT) $(WARN) $(SAN) -fno-exceptions -fno-rtti \
	-Icomponents/a2ui_core/include
TEST_CXXFLAGS := -std=c++17 $(OPT) $(WARN) $(GCC_TEST_WARN) $(SAN) \
	-Icomponents/a2ui_core/include -Itest/harness
LDFLAGS += $(SAN)

CORE_SRCS := $(shell find components/a2ui_core/src -name '*.cpp' | sort)
CORE_OBJS := $(CORE_SRCS:%.cpp=$(BUILD)/%.o)

HARNESS_SRCS := $(shell find test/harness -name '*.cpp' | sort)
HARNESS_OBJS := $(HARNESS_SRCS:%.cpp=$(BUILD)/%.o)

UNIT_SRCS := $(shell find test/unit -name '*.cpp' 2>/dev/null | sort)
UNIT_OBJS := $(UNIT_SRCS:%.cpp=$(BUILD)/%.o)

# LVGL (C only; the C++ sources under src/libs are optional backends we do not
# enable) built against test/lvgl/lv_conf.h, plus the a2ui_lvgl adapter.
LVGL := third_party/lvgl
LVGL_INC := -DLV_CONF_INCLUDE_SIMPLE -Itest/lvgl -I$(LVGL) -I$(LVGL)/include
LVGL_CFLAGS := -std=gnu99 -O1 -g $(SAN) $(LVGL_INC)
LVGL_SRCS := $(shell find $(LVGL)/src -name '*.c' 2>/dev/null | sort)
LVGL_OBJS := $(LVGL_SRCS:$(LVGL)/src/%.c=$(BUILD)/lvgl/%.o)
HAVE_LVGL := $(wildcard $(LVGL)/lvgl.h)

ADAPTER_SRCS := $(shell find components/a2ui_lvgl/src -name '*.cpp' | sort)
ADAPTER_OBJS := $(ADAPTER_SRCS:%.cpp=$(BUILD)/%.o)
LVGL_ENV_OBJ := $(BUILD)/test/lvgl/lvgl_env.o
SMOKE_OBJS := $(BUILD)/test/lvgl/smoke_test.o $(LVGL_ENV_OBJ)
# The adapter conformance runner shares the harness plumbing (not the core suites).
ADAPTER_CONF_OBJS := $(BUILD)/test/lvgl/adapter_suite.o $(LVGL_ENV_OBJ) \
	$(BUILD)/test/harness/harness.o $(BUILD)/test/harness/main.o
RENDER_OBJS := $(BUILD)/tools/render_sample/render_sample.o $(BUILD)/tools/render_sample/png_writer.o $(LVGL_ENV_OBJ)
# The end-to-end test: the transport's platform-neutral session, a test
# WebSocket client and the adapter, against the sample agent (needs uv).
E2E_OBJS := $(BUILD)/test/e2e/agent_e2e.o $(BUILD)/test/e2e/ws_client.o \
	$(BUILD)/examples/agent_surface/components/a2ui_transport_ws/src/agent_session.o $(LVGL_ENV_OBJ)
ADAPTER_SUITES := build/conformance/core/adapter_conformance.json build/conformance/core/accessibility.json \
	build/conformance/adapter/markdown_subset.json
EXAMPLES := $(wildcard $(UPSTREAM)/catalogs/basic/v1/examples/*.json)
# Adapter fixtures (test/lvgl/renders) for layouts the examples do not cover.
EXAMPLES += $(wildcard test/lvgl/renders/*.json)

$(ADAPTER_OBJS): LIB_CXXFLAGS += -Icomponents/a2ui_lvgl/include $(LVGL_INC)
$(BUILD)/test/lvgl/%.o: TEST_CXXFLAGS += -Icomponents/a2ui_lvgl/include -Icomponents/a2ui_lvgl/src $(LVGL_INC)
$(BUILD)/tools/%.o: TEST_CXXFLAGS += -Icomponents/a2ui_lvgl/include -Itest/lvgl $(LVGL_INC)
TRANSPORT := examples/agent_surface/components/a2ui_transport_ws
$(BUILD)/test/e2e/%.o: TEST_CXXFLAGS += -Icomponents/a2ui_lvgl/include -I$(TRANSPORT)/include \
	-Itest/lvgl $(LVGL_INC)

SUITE_YAML := $(wildcard $(UPSTREAM)/conformance/core/*.yaml)
SUITE_JSON := $(SUITE_YAML:$(UPSTREAM)/conformance/%.yaml=build/conformance/%.json)
# Fixtures the node_resolution suite names (test_data/node/*.yaml).
FIXTURE_YAML := $(wildcard $(UPSTREAM)/conformance/test_data/node/*.yaml)
SUITE_JSON += $(FIXTURE_YAML:$(UPSTREAM)/conformance/%.yaml=build/conformance/%.json)
# Adapter-only suites outside core/.
SUITE_JSON += build/conformance/adapter/markdown_subset.json

.PHONY: all test check-refs unit conformance suites lvgl lvgl-smoke adapter-conformance renders simulator e2e heap-report clean

all: $(BUILD)/libA2uiCore.a $(BUILD)/unit_tests $(BUILD)/conformance suites $(if $(HAVE_LVGL),lvgl)

$(BUILD)/components/%.o: components/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(LIB_CXXFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/test/%.o: test/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(TEST_CXXFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/tools/%.o: tools/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(TEST_CXXFLAGS) -MMD -MP -c $< -o $@

# The transport's session code builds like the libraries (no exceptions, no RTTI).
$(BUILD)/$(TRANSPORT)/%.o: $(TRANSPORT)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(LIB_CXXFLAGS) -I$(TRANSPORT)/include -MMD -MP -c $< -o $@

$(BUILD)/lvgl/%.o: $(LVGL)/src/%.c
	@mkdir -p $(dir $@)
	@$(CC) $(LVGL_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/liblvgl.a: $(LVGL_OBJS)
	@rm -f $@
	ar rcs $@ $^

$(BUILD)/libA2uiLvgl.a: $(ADAPTER_OBJS)
	@rm -f $@
	ar rcs $@ $^

$(BUILD)/lvgl_smoke_test: $(SMOKE_OBJS) $(BUILD)/libA2uiLvgl.a $(BUILD)/libA2uiCore.a $(BUILD)/liblvgl.a
	$(CXX) $^ $(LDFLAGS) -o $@

$(BUILD)/adapter_conformance: $(ADAPTER_CONF_OBJS) $(BUILD)/libA2uiLvgl.a $(BUILD)/libA2uiCore.a $(BUILD)/liblvgl.a
	$(CXX) $^ $(LDFLAGS) -o $@

$(BUILD)/render_sample: $(RENDER_OBJS) $(BUILD)/libA2uiLvgl.a $(BUILD)/libA2uiCore.a $(BUILD)/liblvgl.a
	$(CXX) $^ $(LDFLAGS) -o $@

$(BUILD)/agent_e2e: $(E2E_OBJS) $(BUILD)/libA2uiLvgl.a $(BUILD)/libA2uiCore.a $(BUILD)/liblvgl.a
	$(CXX) $^ $(LDFLAGS) -o $@

$(BUILD)/libA2uiCore.a: $(CORE_OBJS)
	@rm -f $@
	ar rcs $@ $^

$(BUILD)/unit_tests: $(UNIT_OBJS) $(BUILD)/libA2uiCore.a
	$(CXX) $(UNIT_OBJS) $(BUILD)/libA2uiCore.a $(LDFLAGS) -o $@

$(BUILD)/conformance: $(HARNESS_OBJS) $(BUILD)/libA2uiCore.a
	$(CXX) $(HARNESS_OBJS) $(BUILD)/libA2uiCore.a $(LDFLAGS) -o $@

build/conformance/%.json: $(UPSTREAM)/conformance/%.yaml tools/yaml2json.py
	@mkdir -p $(dir $@)
	$(UV) run -q --no-project --with pyyaml python tools/yaml2json.py $< $@

suites: $(SUITE_JSON)

unit: $(BUILD)/unit_tests suites
	$(BUILD)/unit_tests

# Each suite runs twice: with Catalog::from_json, then with every catalog
# packed and loaded with Catalog::from_packed (the compact catalog).
CONFORMANCE_ARGS = --repo-root $(UPSTREAM) --suites build/conformance/core $(if $(SUITE),--suite $(SUITE)) \
	$(if $(CASE),--case $(CASE)) $(if $(VERBOSE),--verbose)
conformance: $(BUILD)/conformance suites
	$(BUILD)/conformance $(CONFORMANCE_ARGS)
	$(BUILD)/conformance $(CONFORMANCE_ARGS) --compact

lvgl: $(BUILD)/lvgl_smoke_test $(BUILD)/adapter_conformance $(BUILD)/render_sample

lvgl-smoke: $(BUILD)/lvgl_smoke_test
	$(BUILD)/lvgl_smoke_test

ADAPTER_CONFORMANCE_ARGS = --repo-root $(UPSTREAM) $(foreach f,$(ADAPTER_SUITES),--suite-file $(f)) \
	$(if $(CASE),--case $(CASE)) $(if $(VERBOSE),--verbose)
adapter-conformance: $(BUILD)/adapter_conformance $(ADAPTER_SUITES)
	$(BUILD)/adapter_conformance $(ADAPTER_CONFORMANCE_ARGS)
	$(BUILD)/adapter_conformance $(ADAPTER_CONFORMANCE_ARGS) --compact

# One PNG per basic catalog example and adapter fixture (480x800), for humans to look at.
renders: $(BUILD)/render_sample
	@mkdir -p build/renders
	@for f in $(EXAMPLES); do \
		$(BUILD)/render_sample $$f build/renders/$$(basename $$f .json).png >/dev/null || exit 1; \
	done
	@echo "rendered $(words $(EXAMPLES)) examples into build/renders/"

# The SDL desktop simulator (tools/simulator). Not part of `all`: it needs
# SDL2 and builds its own copy of LVGL, the core and the adapter under
# build/sim with tools/simulator/lv_conf.h (SDL drivers on) and without
# sanitizers. Set SDL2_CFLAGS and SDL2_LIBS to use an SDL2 outside the
# system paths.
SIM := build/sim
SIM_OPT ?= -O2 -g
ifeq ($(origin SDL2_LIBS),undefined)
SDL2_CFLAGS := $(shell pkg-config --cflags sdl2 2>/dev/null || sdl2-config --cflags 2>/dev/null)
SDL2_LIBS := $(shell pkg-config --libs sdl2 2>/dev/null || sdl2-config --libs 2>/dev/null)
endif
SIM_LVGL_INC := -DLV_CONF_INCLUDE_SIMPLE -Itools/simulator -I$(LVGL) -I$(LVGL)/include
SIM_CXXFLAGS := -std=c++17 $(SIM_OPT) $(WARN) -fno-exceptions -fno-rtti \
	-Icomponents/a2ui_core/include -Icomponents/a2ui_lvgl/include $(SIM_LVGL_INC)
SIM_LVGL_OBJS := $(LVGL_SRCS:$(LVGL)/src/%.c=$(SIM)/lvgl/%.o)
SIM_LIB_OBJS := $(CORE_SRCS:%.cpp=$(SIM)/%.o) $(ADAPTER_SRCS:%.cpp=$(SIM)/%.o)
SIM_APP_OBJS := $(SIM)/tools/simulator/simulator.o $(SIM)/tools/render_sample/png_writer.o

$(SIM)/lvgl/%.o: $(LVGL)/src/%.c tools/simulator/lv_conf.h
	@mkdir -p $(dir $@)
	@$(CC) -std=gnu99 $(SIM_OPT) $(SIM_LVGL_INC) $(SDL2_CFLAGS) -MMD -MP -c $< -o $@

$(SIM)/components/%.o: components/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(SIM_CXXFLAGS) -MMD -MP -c $< -o $@

$(SIM)/tools/%.o: tools/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(SIM_CXXFLAGS) -Itools/render_sample $(SDL2_CFLAGS) -MMD -MP -c $< -o $@

$(SIM)/liblvgl.a: $(SIM_LVGL_OBJS)
	@rm -f $@
	ar rcs $@ $^

$(SIM)/libA2uiSim.a: $(SIM_LIB_OBJS)
	@rm -f $@
	ar rcs $@ $^

$(SIM)/a2ui_simulator: $(SIM_APP_OBJS) $(SIM)/libA2uiSim.a $(SIM)/liblvgl.a
	$(CXX) $^ $(SDL2_LIBS) -o $@

ifeq ($(strip $(SDL2_LIBS)),)
simulator:
	@echo "simulator: SDL2 not found (install libsdl2-dev, or set SDL2_CFLAGS and SDL2_LIBS)"; exit 1
else ifeq ($(HAVE_LVGL),)
simulator:
	@echo "simulator: $(LVGL) not checked out (git submodule update --init $(LVGL))"; exit 1
else
simulator: $(SIM)/a2ui_simulator
	@echo "built $(SIM)/a2ui_simulator"
endif

e2e: $(BUILD)/agent_e2e
	UV=$(UV) test/e2e/run.sh $(BUILD)/agent_e2e

# Counts live heap with its own operator new, so it builds without sanitizers
# in a separate directory.
HEAP_BUILD := build/heap
$(BUILD)/heap_report: $(BUILD)/tools/heap_report/heap_report.o $(BUILD)/libA2uiCore.a
	$(CXX) $^ $(LDFLAGS) -o $@

heap-report:
	@$(MAKE) --no-print-directory BUILD=$(HEAP_BUILD) SAN= OPT="-O2 -g" $(HEAP_BUILD)/heap_report
	$(HEAP_BUILD)/heap_report
	@echo
	$(HEAP_BUILD)/heap_report --load
	@echo
	$(HEAP_BUILD)/heap_report --compact

# The libraries use a2ui::Ref (a2ui/ref.h), not std::shared_ptr: on ESP-IDF
# every std::shared_ptr control block creates a FreeRTOS mutex in internal
# RAM. User-facing handles stay std::shared_ptr<void>; mark any other
# deliberate use with "ref-check: allow".
REF_CHECK_DIRS ?= components
check-refs:
	@bad=$$(grep -rnE 'std::(shared_ptr|weak_ptr|make_shared|allocate_shared|enable_shared_from_this)\b' \
		$(REF_CHECK_DIRS) --include='*.h' --include='*.cpp' --include='*.inc' \
		| grep -v 'a2ui_core/include/a2ui/ref.h:' \
		| grep -v 'ref-check: allow' \
		| grep -v 'std::shared_ptr<void>' || true); \
	if [ -n "$$bad" ]; then \
		echo "$$bad"; \
		echo "check-refs: use a2ui::Ref and a2ui::make_ref (a2ui/ref.h) in components/"; \
		exit 1; \
	fi
	@echo "check-refs: ok"

ifneq ($(HAVE_LVGL),)
test: check-refs unit conformance lvgl-smoke adapter-conformance
else
test: check-refs unit conformance
	@echo "note: $(LVGL) not checked out; skipping the LVGL smoke test"
endif

clean:
	rm -rf build

-include $(CORE_OBJS:.o=.d) $(HARNESS_OBJS:.o=.d) $(UNIT_OBJS:.o=.d)
-include $(LVGL_OBJS:.o=.d) $(ADAPTER_OBJS:.o=.d) $(SMOKE_OBJS:.o=.d) $(ADAPTER_CONF_OBJS:.o=.d) $(RENDER_OBJS:.o=.d)
-include $(SIM_LVGL_OBJS:.o=.d) $(SIM_LIB_OBJS:.o=.d) $(SIM_APP_OBJS:.o=.d)
-include $(E2E_OBJS:.o=.d)

-include $(BUILD)/tools/heap_report/heap_report.d
