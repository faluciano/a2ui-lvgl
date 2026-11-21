# Host build for the A2UI core and LVGL adapter, plus the conformance harness.
#
#   make            build the libraries and test binaries
#   make test       run unit tests and the conformance suites
#   make conformance SUITE=data_model   run one suite (substring match)
#   make lvgl-smoke  build LVGL (third_party/lvgl) and run the LVGL adapter tests
#   make adapter-conformance   run the framework adapter suites against a2ui_lvgl
#   make renders     render catalogs/basic/v1/examples to build/renders/*.png
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
ADAPTER_SUITES := build/conformance/core/adapter_conformance.json build/conformance/core/accessibility.json \
	build/conformance/adapter/markdown_subset.json
EXAMPLES := $(wildcard $(UPSTREAM)/catalogs/basic/v1/examples/*.json)

$(ADAPTER_OBJS): LIB_CXXFLAGS += -Icomponents/a2ui_lvgl/include $(LVGL_INC)
$(BUILD)/test/lvgl/%.o: TEST_CXXFLAGS += -Icomponents/a2ui_lvgl/include -Icomponents/a2ui_lvgl/src $(LVGL_INC)
$(BUILD)/tools/%.o: TEST_CXXFLAGS += -Icomponents/a2ui_lvgl/include -Itest/lvgl $(LVGL_INC)

SUITE_YAML := $(wildcard $(UPSTREAM)/conformance/core/*.yaml)
SUITE_JSON := $(SUITE_YAML:$(UPSTREAM)/conformance/%.yaml=build/conformance/%.json)
# Fixtures the node_resolution suite names (test_data/node/*.yaml).
FIXTURE_YAML := $(wildcard $(UPSTREAM)/conformance/test_data/node/*.yaml)
SUITE_JSON += $(FIXTURE_YAML:$(UPSTREAM)/conformance/%.yaml=build/conformance/%.json)
# Adapter-only suites outside core/.
SUITE_JSON += build/conformance/adapter/markdown_subset.json

.PHONY: all test unit conformance suites lvgl lvgl-smoke adapter-conformance renders clean

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

conformance: $(BUILD)/conformance suites
	$(BUILD)/conformance --repo-root $(UPSTREAM) --suites build/conformance/core $(if $(SUITE),--suite $(SUITE)) $(if $(CASE),--case $(CASE)) $(if $(VERBOSE),--verbose)

lvgl: $(BUILD)/lvgl_smoke_test $(BUILD)/adapter_conformance $(BUILD)/render_sample

lvgl-smoke: $(BUILD)/lvgl_smoke_test
	$(BUILD)/lvgl_smoke_test

adapter-conformance: $(BUILD)/adapter_conformance $(ADAPTER_SUITES)
	$(BUILD)/adapter_conformance --repo-root $(UPSTREAM) $(foreach f,$(ADAPTER_SUITES),--suite-file $(f)) \
		$(if $(CASE),--case $(CASE)) $(if $(VERBOSE),--verbose)

# One PNG per basic catalog example (480x800), for humans to look at.
renders: $(BUILD)/render_sample
	@mkdir -p build/renders
	@for f in $(EXAMPLES); do \
		$(BUILD)/render_sample $$f build/renders/$$(basename $$f .json).png >/dev/null || exit 1; \
	done
	@echo "rendered $(words $(EXAMPLES)) examples into build/renders/"

ifneq ($(HAVE_LVGL),)
test: unit conformance lvgl-smoke adapter-conformance
else
test: unit conformance
	@echo "note: $(LVGL) not checked out; skipping the LVGL smoke test"
endif

clean:
	rm -rf build

-include $(CORE_OBJS:.o=.d) $(HARNESS_OBJS:.o=.d) $(UNIT_OBJS:.o=.d)
-include $(LVGL_OBJS:.o=.d) $(ADAPTER_OBJS:.o=.d) $(SMOKE_OBJS:.o=.d) $(ADAPTER_CONF_OBJS:.o=.d) $(RENDER_OBJS:.o=.d)
