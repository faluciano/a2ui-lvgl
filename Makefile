# Host build for the A2UI core and LVGL adapter, plus the conformance harness.
#
#   make            build the libraries and test binaries
#   make test       run unit tests and the conformance suites
#   make conformance SUITE=data_model   run one suite (substring match)
#   make lvgl-smoke  build LVGL (third_party/lvgl) and run the LVGL smoke test
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
WARN := -Wall -Wextra -Wno-unused-parameter -Wformat=2 -Wformat-truncation=2 -Wformat-overflow=2

LIB_CXXFLAGS := -std=c++17 $(OPT) $(WARN) $(SAN) -fno-exceptions -fno-rtti \
	-Icomponents/a2ui_core/include
TEST_CXXFLAGS := -std=c++17 $(OPT) $(WARN) -Wno-maybe-uninitialized $(SAN) \
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
SMOKE_OBJS := $(BUILD)/test/lvgl/smoke_test.o

$(ADAPTER_OBJS): LIB_CXXFLAGS += -Icomponents/a2ui_lvgl/include $(LVGL_INC)
$(SMOKE_OBJS): TEST_CXXFLAGS += -Icomponents/a2ui_lvgl/include $(LVGL_INC)

SUITE_YAML := $(wildcard $(UPSTREAM)/conformance/core/*.yaml)
SUITE_JSON := $(SUITE_YAML:$(UPSTREAM)/conformance/%.yaml=build/conformance/%.json)

.PHONY: all test unit conformance suites lvgl lvgl-smoke clean

all: $(BUILD)/libA2uiCore.a $(BUILD)/unit_tests $(BUILD)/conformance suites $(if $(HAVE_LVGL),lvgl)

$(BUILD)/components/%.o: components/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(LIB_CXXFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/test/%.o: test/%.cpp
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

lvgl: $(BUILD)/lvgl_smoke_test

lvgl-smoke: $(BUILD)/lvgl_smoke_test
	$(BUILD)/lvgl_smoke_test

ifneq ($(HAVE_LVGL),)
test: unit conformance lvgl-smoke
else
test: unit conformance
	@echo "note: $(LVGL) not checked out; skipping the LVGL smoke test"
endif

clean:
	rm -rf build

-include $(CORE_OBJS:.o=.d) $(HARNESS_OBJS:.o=.d) $(UNIT_OBJS:.o=.d)
-include $(LVGL_OBJS:.o=.d) $(ADAPTER_OBJS:.o=.d) $(SMOKE_OBJS:.o=.d)
