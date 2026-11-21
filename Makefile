# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alejandro Bustos

PREFIX ?= /usr/local
LV2DIR ?= $(PREFIX)/lib/lv2
DESTDIR ?=
OPT ?= -O2
PYTHON ?= python3
PKG_CONFIG ?= pkg-config
SAN ?= 0
BUILD_DIR ?= build
ifeq ($(SAN),1)
override BUILD_DIR := $(BUILD_DIR)/san
SAN_FLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer -g
endif
ifeq ($(SAN),thread)
override BUILD_DIR := $(BUILD_DIR)/tsan
SAN_FLAGS := -fsanitize=thread -fno-omit-frame-pointer -g
endif

URIS := https://github.com/ABustos2606/reel\#reel https://github.com/ABustos2606/reel\#lfo
BUNDLE := $(BUILD_DIR)/reel.lv2
VENDOR := third_party/eurorack
SOURCES := src/lv2_entry.cpp src/reel.cpp src/reel_lfo.cpp src/keyframe_state.cpp src/editing.cpp
VENDOR_SOURCES := $(addprefix $(VENDOR)/frames/,keyframer.cc poly_lfo.cc resources.cc)
OBJECTS := $(patsubst src/%.cpp,$(BUILD_DIR)/obj/%.o,$(SOURCES))
VENDOR_OBJECTS := $(patsubst $(VENDOR)/%.cc,$(BUILD_DIR)/obj/vendor/%.o,$(VENDOR_SOURCES))
METADATA := manifest.ttl reel.ttl reel_lfo.ttl
TTL_TARGETS := $(addprefix $(BUNDLE)/,$(METADATA))
LV2_CFLAGS = $(shell $(PKG_CONFIG) --cflags lv2)
# TEST compiles out the keyframer's flash storage, as upstream's own desktop
# builds do (third_party/eurorack/README.md). -fno-fast-math comes after OPT:
# the non-finite guards need IEEE arithmetic even if a host build passes
# -ffast-math in OPT.
BASE_FLAGS = $(CPPFLAGS) $(CXXFLAGS) $(OPT) -std=c++17 -fPIC -fvisibility=hidden -DTEST \
	-fno-fast-math -fno-math-errno -fno-trapping-math $(SAN_FLAGS)
COMPILE_FLAGS = $(BASE_FLAGS) $(LV2_CFLAGS) -isystem $(VENDOR) -Wall -Wextra
# Vendored code is kept as upstream wrote it; its warnings are not ours.
VENDOR_FLAGS = $(BASE_FLAGS) -I$(VENDOR) -w
TEST_LIBS = -ldl -pthread
TESTS := test_scaffold test_keyframes

.PHONY: all install test test-timing test-hosts lint clean
all: $(BUNDLE)/reel.so $(TTL_TARGETS)

$(BUILD_DIR)/obj/%.o: src/%.cpp
	@mkdir -p "$(@D)"
	$(CXX) $(COMPILE_FLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/obj/vendor/%.o: $(VENDOR)/%.cc
	@mkdir -p "$(@D)"
	$(CXX) $(VENDOR_FLAGS) -MMD -MP -c $< -o $@

$(BUNDLE)/reel.so: $(OBJECTS) $(VENDOR_OBJECTS) src/exports.map
	@mkdir -p "$(@D)"
	$(CXX) $(OBJECTS) $(VENDOR_OBJECTS) $(LDFLAGS) $(SAN_FLAGS) -shared -Wl,--no-undefined \
		-Wl,--version-script=src/exports.map -o $@

$(BUNDLE)/%.ttl: reel.lv2/%.ttl
	@mkdir -p "$(@D)"
	install -m 644 $< $@

# The state parser is linked in directly as well (with the Keyframer it
# loads lists into), to drive it without a host.
$(BUILD_DIR)/test_scaffold: tests/test_scaffold.cpp tests/host.cpp tests/host.hpp src/ports.hpp \
		$(BUILD_DIR)/obj/keyframe_state.o $(VENDOR_OBJECTS)
	@mkdir -p "$(@D)"
	$(CXX) $(COMPILE_FLAGS) tests/test_scaffold.cpp tests/host.cpp $(BUILD_DIR)/obj/keyframe_state.o \
		$(VENDOR_OBJECTS) $(LDFLAGS) $(TEST_LIBS) -o $@

# The keyframe tests drive the vendored Keyframer directly too, to compare
# the plugin with it.
$(BUILD_DIR)/test_keyframes: tests/test_keyframes.cpp tests/host.cpp tests/host.hpp src/ports.hpp src/vca.hpp \
		$(BUILD_DIR)/obj/keyframe_state.o $(VENDOR_OBJECTS)
	@mkdir -p "$(@D)"
	$(CXX) $(COMPILE_FLAGS) tests/test_keyframes.cpp tests/host.cpp $(BUILD_DIR)/obj/keyframe_state.o \
		$(VENDOR_OBJECTS) $(LDFLAGS) $(TEST_LIBS) -o $@

test: all $(addprefix $(BUILD_DIR)/,$(TESTS))
	$(PYTHON) tests/ttl_check.py $(BUNDLE)
	$(BUILD_DIR)/test_scaffold $(BUNDLE)/reel.so
	$(BUILD_DIR)/test_keyframes $(BUNDLE)/reel.so

# rt_timing: 10 simulated minutes of random controls, edits and saves.
test-timing: all $(BUILD_DIR)/test_keyframes
	$(BUILD_DIR)/test_keyframes $(BUNDLE)/reel.so --timing

# jalv and Ingen with a private dummy JACK server; Ingen must be on PATH
# (Peepo's tools/ingen-spike/env.sh sets it up).
test-hosts: all
	$(PYTHON) tests/host_smoke.py $(BUNDLE)

install: all
	install -d "$(DESTDIR)$(LV2DIR)/reel.lv2"
	install -m 755 $(BUNDLE)/reel.so "$(DESTDIR)$(LV2DIR)/reel.lv2/"
	install -m 644 $(TTL_TARGETS) LICENSE THIRD_PARTY_NOTICES "$(DESTDIR)$(LV2DIR)/reel.lv2/"

lint:
	@if command -v lv2lint >/dev/null 2>&1; then \
		for uri in $(URIS); do \
			LV2_PATH="$(DESTDIR)$(LV2DIR):$${LV2_PATH:-/usr/local/lib/lv2:/usr/lib/lv2}" lv2lint "$$uri" || exit 1; \
		done; \
	else echo 'SKIP lv2lint: not installed (install the bundle with make install first)'; fi

clean:
	$(RM) -r $(BUILD_DIR)/obj $(BUNDLE) $(addprefix $(BUILD_DIR)/,$(TESTS))

-include $(OBJECTS:.o=.d) $(VENDOR_OBJECTS:.o=.d)
