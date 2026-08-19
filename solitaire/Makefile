# Basic out-of-tree Neutrino userspace package template.
#
# Usage:
#   make
#   make PROGRAM=mytool
#   make SOURCES="src/main.cpp src/extra.cpp"

SHELL := /bin/bash

PROGRAM ?= $(notdir $(CURDIR))

NEUTRINO_ROOT ?= ../../neutrino
NEUTRINO_TARGET ?= x86_64-elf
NEUTRINO_SYSROOT ?= $(abspath $(NEUTRINO_ROOT)/userspace/build/newlib-pie-sysroot)
NEUTRINO_TARGET_ROOT := $(NEUTRINO_SYSROOT)/$(NEUTRINO_TARGET)

CXX = $(NEUTRINO_TARGET)-g++

BUILD_DIR ?= build/newlib
OUT_DIR   ?= out
PACKAGE_DIR ?= package

SRC_DIR     := src
LIB_DIR     := lib
INCLUDE_DIR := include

SOURCES ?= $(sort $(wildcard $(SRC_DIR)/*.cpp))
SUPPORT_SOURCES := $(filter-out $(LIB_DIR)/ctype.cpp $(LIB_DIR)/string.cpp,\
                   $(sort $(wildcard $(LIB_DIR)/*.cpp)))

PROGRAM_OBJECTS := $(patsubst %.cpp,$(BUILD_DIR)/%.o,$(SOURCES))
SUPPORT_OBJECTS := $(patsubst %.cpp,$(BUILD_DIR)/%.o,$(SUPPORT_SOURCES))
OBJECTS := $(SUPPORT_OBJECTS) $(PROGRAM_OBJECTS)

NEWLIB_CRT0 := $(NEUTRINO_TARGET_ROOT)/lib/crt0.o
NEWLIB_LIBNEUTRINO := $(NEUTRINO_TARGET_ROOT)/lib/libneutrino.a
NEWLIB_LIBC := $(NEUTRINO_TARGET_ROOT)/lib/libc.a
NEWLIB_LIBM := $(NEUTRINO_TARGET_ROOT)/lib/libm.a
NEWLIB_FILES := $(NEWLIB_CRT0) $(NEWLIB_LIBNEUTRINO) $(NEWLIB_LIBC) $(NEWLIB_LIBM)
NEWLIB_LIBS := -Wl,--start-group $(NEWLIB_LIBNEUTRINO) \
               $(NEWLIB_LIBC) $(NEWLIB_LIBM) -lgcc -Wl,--end-group

TARGET := $(OUT_DIR)/$(PROGRAM).elf
PACKAGE_ROOT := $(BUILD_DIR)/pkgroot
PACKAGE_ZIP := $(OUT_DIR)/$(PROGRAM).zip

CXXFLAGS ?= -std=c++20 -O2 -ffreestanding -fno-builtin -fno-stack-protector \
            -nostdlib -m64 -mno-red-zone -mno-mmx -mno-sse -mno-sse2 \
            -mno-avx -mno-avx512f -fPIE -Wall -Wextra -D_DEFAULT_SOURCE \
            -I$(INCLUDE_DIR) \
            -isystem $(NEUTRINO_SYSROOT)/include \
            -isystem $(NEUTRINO_TARGET_ROOT)/include
LDFLAGS ?= -pie -Wl,--no-dynamic-linker,-z,noexecstack

# THIS IS NEUTRINO BITCH WE -ffreestanding -fno-builtin -fno-stack-protector
# -mno-mmx -mno-sse -mno-sse2 -mno-avx -mno-avx512f IN THIS MUTHAFUCKA
# BETTER TAKE YO SENSITIVE ASS BACK TO WIN32

.PHONY: all package clean

all: $(TARGET)

$(TARGET): $(OBJECTS) $(NEWLIB_FILES) Makefile | $(OUT_DIR)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $(NEWLIB_CRT0) $(OBJECTS) \
		$(NEWLIB_LIBS) -o $@

package: $(TARGET) $(PACKAGE_DIR)/manifest.toml | $(OUT_DIR)
	rm -rf $(PACKAGE_ROOT)
	mkdir -p $(PACKAGE_ROOT)/binary $(PACKAGE_ROOT)/config $(PACKAGE_ROOT)/scripts
	cp $(TARGET) $(PACKAGE_ROOT)/binary/$(PROGRAM).elf
	cp $(PACKAGE_DIR)/manifest.toml $(PACKAGE_ROOT)/manifest.toml
	if [[ -d "$(PACKAGE_DIR)/config" ]]; then cp -a "$(PACKAGE_DIR)/config/." "$(PACKAGE_ROOT)/config/"; fi
	rm -f $(PACKAGE_ZIP)
	cd $(PACKAGE_ROOT) && zip -0 -r "$(abspath $(PACKAGE_ZIP))" manifest.toml binary config scripts

$(BUILD_DIR)/%.o: %.cpp Makefile | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(NEWLIB_FILES):
	@echo "Neutrino Newlib SDK is missing: $@" >&2
	@echo "Run: make -C $(NEUTRINO_ROOT)/userspace newlib-sdk" >&2
	@false

$(BUILD_DIR) $(OUT_DIR):
	mkdir -p $@

clean:
	rm -rf $(BUILD_DIR) $(OUT_DIR)
