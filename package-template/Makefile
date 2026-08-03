# Basic out-of-tree Neutrino userspace package template.
#
# Usage:
#   make
#   make PROGRAM=mytool
#   make SOURCES="src/main.cpp src/extra.cpp"

SHELL := /bin/bash

PROGRAM ?= $(notdir $(CURDIR))

CXX  ?= g++
NASM ?= nasm

BUILD_DIR ?= build
OUT_DIR   ?= out
PACKAGE_DIR ?= package

SRC_DIR     := src
CRT_DIR     := crt
LIB_DIR     := lib
INCLUDE_DIR := include

SOURCES ?= $(sort $(wildcard $(SRC_DIR)/*.cpp))
SUPPORT_SOURCES := $(sort $(wildcard $(LIB_DIR)/*.cpp))

CRT_OBJECT := $(BUILD_DIR)/crt0.o
PROGRAM_OBJECTS := $(patsubst %.cpp,$(BUILD_DIR)/%.o,$(SOURCES))
SUPPORT_OBJECTS := $(patsubst %.cpp,$(BUILD_DIR)/%.o,$(SUPPORT_SOURCES))
OBJECTS := $(CRT_OBJECT) $(SUPPORT_OBJECTS) $(PROGRAM_OBJECTS)

TARGET := $(OUT_DIR)/$(PROGRAM).elf
PACKAGE_ROOT := $(BUILD_DIR)/pkgroot
PACKAGE_ZIP := $(OUT_DIR)/$(PROGRAM).zip

ASFLAGS ?= -f elf64
CXXFLAGS ?= -std=c++20 -ffreestanding -fno-builtin -fno-stack-protector \
            -nostdlib -m64 -mno-red-zone -mno-mmx -mno-sse -mno-sse2 \
            -mno-avx -mno-avx512f -fPIE -pie -Wall -Wextra \
            -I$(INCLUDE_DIR)

# THIS IS NEUTRINO BITCH WE -ffreestanding -fno-builtin -fno-stack-protector
# -mno-mmx -mno-sse -mno-sse2 -mno-avx -mno-avx512f IN THIS MUTHAFUCKA
# BETTER TAKE YO SENSITIVE ASS BACK TO WIN32

.PHONY: all package clean

all: $(TARGET)

$(TARGET): $(OBJECTS) | $(OUT_DIR)
	$(CXX) $(CXXFLAGS) $(OBJECTS) -o $@

package: $(TARGET) $(PACKAGE_DIR)/manifest.toml | $(OUT_DIR)
	rm -rf $(PACKAGE_ROOT)
	mkdir -p $(PACKAGE_ROOT)/binary $(PACKAGE_ROOT)/config $(PACKAGE_ROOT)/scripts
	cp $(TARGET) $(PACKAGE_ROOT)/binary/$(PROGRAM).elf
	cp $(PACKAGE_DIR)/manifest.toml $(PACKAGE_ROOT)/manifest.toml
	if [[ -d "$(PACKAGE_DIR)/config" ]]; then cp -a "$(PACKAGE_DIR)/config/." "$(PACKAGE_ROOT)/config/"; fi
	rm -f $(PACKAGE_ZIP)
	cd $(PACKAGE_ROOT) && zip -0 -r "$(abspath $(PACKAGE_ZIP))" manifest.toml binary config scripts

$(BUILD_DIR)/crt0.o: $(CRT_DIR)/crt0.s | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(NASM) $(ASFLAGS) $< -o $@

$(BUILD_DIR)/%.o: %.cpp | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR) $(OUT_DIR):
	mkdir -p $@

clean:
	rm -rf $(BUILD_DIR) $(OUT_DIR)
