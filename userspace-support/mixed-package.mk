# Shared build recipe for packages containing both Newlib C and legacy C++ tools.
SHELL := /bin/bash
NEUTRINO_ROOT ?= ../../neutrino
SUPPORT_ROOT ?= ../userspace-support
NEUTRINO_TARGET ?= x86_64-elf
NEUTRINO_SYSROOT ?= $(abspath $(NEUTRINO_ROOT)/userspace/build/newlib-pie-sysroot)
NEUTRINO_TARGET_ROOT := $(NEUTRINO_SYSROOT)/$(NEUTRINO_TARGET)
BUILD_DIR ?= build
OUT_DIR ?= out
PACKAGE_DIR ?= package
NEWLIB_CC ?= $(NEUTRINO_TARGET)-gcc
CXX ?= g++
NASM ?= nasm
PROGRAMS := $(C_PROGRAMS) $(CPP_PROGRAMS)
PROGRAM_TARGETS := $(addprefix $(OUT_DIR)/,$(addsuffix .elf,$(PROGRAMS)))
NEWLIB_CRT0 := $(NEUTRINO_TARGET_ROOT)/lib/crt0.o
NEWLIB_LIBNEUTRINO := $(NEUTRINO_TARGET_ROOT)/lib/libneutrino.a
NEWLIB_LIBC := $(NEUTRINO_TARGET_ROOT)/lib/libc.a
NEWLIB_LIBM := $(NEUTRINO_TARGET_ROOT)/lib/libm.a
NEWLIB_FILES := $(NEWLIB_CRT0) $(NEWLIB_LIBNEUTRINO) $(NEWLIB_LIBC) $(NEWLIB_LIBM)
NEWLIB_LIBS := -Wl,--start-group $(NEWLIB_LIBNEUTRINO) $(NEWLIB_LIBC) $(NEWLIB_LIBM) -lgcc -Wl,--end-group
LEGACY_CRT := $(BUILD_DIR)/legacy/crt0.o
LEGACY_LIBC_SOURCES := $(SUPPORT_ROOT)/libc/ctype.cpp $(SUPPORT_ROOT)/libc/neutrino.cpp $(SUPPORT_ROOT)/libc/string.cpp
LEGACY_LIBC_OBJECTS := $(patsubst $(SUPPORT_ROOT)/%.cpp,$(BUILD_DIR)/legacy/support/%.o,$(LEGACY_LIBC_SOURCES))
PACKAGE_ROOT := $(BUILD_DIR)/pkgroot
PACKAGE_ZIP := $(OUT_DIR)/$(PACKAGE).zip
CFLAGS ?= -O2 -g -ffreestanding -fno-builtin -fno-stack-protector -nostdlib -m64 -mno-red-zone -mno-avx -mno-avx512f -fPIE -std=c11 -Wall -Wextra -Wpedantic -isystem $(NEUTRINO_SYSROOT)/include -isystem $(NEUTRINO_TARGET_ROOT)/include
CXXFLAGS ?= -std=c++20 -O2 -ffreestanding -fno-builtin -fno-stack-protector -nostdlib -m64 -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -mno-avx -mno-avx512f -fPIE -pie -fno-gnu-unique -Wall -Wextra -I$(SUPPORT_ROOT)/helpers -I$(SUPPORT_ROOT)/crt -I$(SUPPORT_ROOT)/libc/include -I$(NEUTRINO_ROOT)/shared/include
.PHONY: all package clean
all: $(PROGRAM_TARGETS)
$(addprefix $(OUT_DIR)/,$(addsuffix .elf,$(C_PROGRAMS))): $(OUT_DIR)/%.elf: $(BUILD_DIR)/newlib/%.o $(NEWLIB_FILES) | $(OUT_DIR)
	$(NEWLIB_CC) $(CFLAGS) -pie -Wl,--no-dynamic-linker,-z,noexecstack $(NEWLIB_CRT0) $< $(NEWLIB_LIBS) -o $@
$(addprefix $(OUT_DIR)/,$(addsuffix .elf,$(CPP_PROGRAMS))): $(OUT_DIR)/%.elf: $(BUILD_DIR)/legacy/src/%.o $(LEGACY_CRT) $(LEGACY_LIBC_OBJECTS) | $(OUT_DIR)
	$(CXX) $(CXXFLAGS) -Wl,--no-dynamic-linker $(LEGACY_CRT) $(LEGACY_LIBC_OBJECTS) $< -o $@
package: $(PROGRAM_TARGETS) $(PACKAGE_DIR)/manifest.toml | $(OUT_DIR)
	rm -rf $(PACKAGE_ROOT)
	mkdir -p $(PACKAGE_ROOT)/binary $(PACKAGE_ROOT)/config $(PACKAGE_ROOT)/scripts
	for prog in $(PROGRAMS); do cp $(OUT_DIR)/$$prog.elf $(PACKAGE_ROOT)/binary/$$prog.elf; done
	cp $(PACKAGE_DIR)/manifest.toml $(PACKAGE_ROOT)/manifest.toml
	rm -f $(PACKAGE_ZIP)
	cd $(PACKAGE_ROOT) && zip -0 -r "$(abspath $(PACKAGE_ZIP))" manifest.toml binary config scripts
$(BUILD_DIR)/newlib/%.o: src/%.c $(NEWLIB_FILES)
	@mkdir -p $(dir $@)
	$(NEWLIB_CC) $(CFLAGS) -c $< -o $@
$(BUILD_DIR)/legacy/src/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@
$(BUILD_DIR)/legacy/crt0.o: $(SUPPORT_ROOT)/crt/crt0.s
	@mkdir -p $(dir $@)
	$(NASM) -f elf64 $< -o $@
$(BUILD_DIR)/legacy/support/%.o: $(SUPPORT_ROOT)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@
$(NEWLIB_FILES):
	@echo "Neutrino Newlib SDK is missing: $@" >&2
	@false
$(OUT_DIR):
	mkdir -p $@
clean:
	rm -rf $(BUILD_DIR) $(OUT_DIR)
