# Host build for the A2UI core and LVGL adapter, plus the conformance harness.
#
#   make            build the libraries and test binaries
#   make test       run unit tests and the conformance suites
#   make conformance SUITE=data_model   run one suite (substring match)
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
WARN := -Wall -Wextra -Wno-unused-parameter

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

SUITE_YAML := $(wildcard $(UPSTREAM)/conformance/core/*.yaml)
SUITE_JSON := $(SUITE_YAML:$(UPSTREAM)/conformance/%.yaml=build/conformance/%.json)

.PHONY: all test unit conformance suites clean

all: $(BUILD)/libA2uiCore.a $(BUILD)/unit_tests $(BUILD)/conformance suites

$(BUILD)/components/%.o: components/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(LIB_CXXFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/test/%.o: test/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(TEST_CXXFLAGS) -MMD -MP -c $< -o $@

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

test: unit conformance

clean:
	rm -rf build

-include $(CORE_OBJS:.o=.d) $(HARNESS_OBJS:.o=.d) $(UNIT_OBJS:.o=.d)
