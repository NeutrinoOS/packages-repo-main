# Shared build recipe for freestanding C++ Neutrino packages.

SHELL := /bin/bash

NEUTRINO_ROOT ?= ../../neutrino
SUPPORT_ROOT ?= ../userspace-support
PACKAGE_DIR ?= package
BUILD_DIR ?= build
OUT_DIR ?= out

CXX ?= g++
NASM ?= nasm

CRT_OBJECT := $(BUILD_DIR)/crt0.o
LIBC_SOURCES := $(SUPPORT_ROOT)/libc/ctype.cpp \
                $(SUPPORT_ROOT)/libc/neutrino.cpp \
                $(SUPPORT_ROOT)/libc/string.cpp
LIBC_OBJECTS := $(patsubst $(SUPPORT_ROOT)/%.cpp,$(BUILD_DIR)/support/%.o,$(LIBC_SOURCES))
HELPER_SOURCES := $(addprefix $(SUPPORT_ROOT)/helpers/,$(addsuffix .cpp,$(SUPPORT_HELPERS)))
HELPER_OBJECTS := $(patsubst $(SUPPORT_ROOT)/%.cpp,$(BUILD_DIR)/support/%.o,$(HELPER_SOURCES))
PROGRAM_OBJECTS := $(addprefix $(BUILD_DIR)/src/,$(addsuffix .o,$(PROGRAMS)))
DEPENDENCY_FILES := $(PROGRAM_OBJECTS:.o=.d) $(LIBC_OBJECTS:.o=.d) $(HELPER_OBJECTS:.o=.d)
SUPPORT_BUILD_MK := $(lastword $(MAKEFILE_LIST))
PROGRAM_TARGETS := $(addprefix $(OUT_DIR)/,$(addsuffix .elf,$(PROGRAMS)))
PACKAGE_ROOT := $(BUILD_DIR)/pkgroot
PACKAGE_ZIP := $(OUT_DIR)/$(PACKAGE).zip

ASFLAGS ?= -f elf64
CXXFLAGS ?= -std=c++20 -O2 -ffreestanding -fno-builtin -fno-stack-protector \
            -nostdlib -m64 -mno-red-zone -mno-mmx -mno-sse -mno-sse2 \
            -mno-avx -mno-avx512f -fPIE -pie -fno-gnu-unique -Wall -Wextra \
            -I$(SUPPORT_ROOT)/helpers -I$(SUPPORT_ROOT)/crt -I$(SUPPORT_ROOT)/libc/include \
            -I$(NEUTRINO_ROOT)/shared/include -I../bearssl/include/bearssl \
            -I../libnet/include
LDFLAGS ?= -Wl,--no-dynamic-linker
EXTRA_LIBS ?=
LIBNET_LIBRARY ?= ../libnet/library/libnet.so.0

.PHONY: all package clean
.SECONDARY: $(PROGRAM_OBJECTS)

all: $(PROGRAM_TARGETS)

ifneq ($(filter $(LIBNET_LIBRARY),$(EXTRA_LIBS)),)
$(PROGRAM_TARGETS): $(LIBNET_LIBRARY)
$(LIBNET_LIBRARY):
	$(MAKE) -C ../libnet
endif

$(OUT_DIR)/%.elf: $(BUILD_DIR)/src/%.o $(CRT_OBJECT) $(LIBC_OBJECTS) $(HELPER_OBJECTS) | $(OUT_DIR)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $(CRT_OBJECT) $(LIBC_OBJECTS) $(HELPER_OBJECTS) $< $(EXTRA_LIBS) -o $@

package: $(PROGRAM_TARGETS) $(PACKAGE_DIR)/manifest.toml | $(OUT_DIR)
	rm -rf $(PACKAGE_ROOT)
	mkdir -p $(PACKAGE_ROOT)/binary $(PACKAGE_ROOT)/config $(PACKAGE_ROOT)/scripts
	for prog in $(PROGRAMS); do cp $(OUT_DIR)/$$prog.elf $(PACKAGE_ROOT)/binary/$$prog.elf; done
	cp $(PACKAGE_DIR)/manifest.toml $(PACKAGE_ROOT)/manifest.toml
	if [[ -d "$(PACKAGE_DIR)/config" ]]; then cp -a "$(PACKAGE_DIR)/config/." "$(PACKAGE_ROOT)/config/"; fi
	$(PACKAGE_EXTRA_COMMANDS)
	rm -f $(PACKAGE_ZIP)
	cd $(PACKAGE_ROOT) && zip -0 -r "$(abspath $(PACKAGE_ZIP))" manifest.toml binary config scripts $(PACKAGE_EXTRA_PATHS)

$(BUILD_DIR)/crt0.o: $(SUPPORT_ROOT)/crt/crt0.s | $(BUILD_DIR)
	$(NASM) $(ASFLAGS) $< -o $@

$(BUILD_DIR)/src/%.o: src/%.cpp $(SUPPORT_BUILD_MK) | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/support/%.o: $(SUPPORT_ROOT)/%.cpp $(SUPPORT_BUILD_MK) | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR) $(OUT_DIR):
	mkdir -p $@

clean:
	rm -rf $(BUILD_DIR) $(OUT_DIR)

-include $(DEPENDENCY_FILES)
