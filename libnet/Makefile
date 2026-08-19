# Neutrino networking and HTTP shared library.
#
# Provides libnet.so.0 with the stable net and http client/server APIs.
# Packaged headers live under include/neutrino/.

SHELL := /bin/bash

PACKAGE := libnet
SONAME := libnet.so.0
LINKER_NAME := libnet.so

CXX ?= g++
ZIP ?= zip

NEUTRINO_ROOT ?= ../../neutrino
SUPPORT_ROOT ?= ../userspace-support
BUILD_DIR ?= build
OUT_DIR ?= out
PACKAGE_DIR ?= package
INCLUDE_DIR := include
LIBRARY_DIR := library

SOURCES := src/http.cpp src/http_client.cpp src/http_server.cpp src/net.cpp
OBJECTS := $(patsubst src/%.cpp,$(BUILD_DIR)/src/%.o,$(SOURCES))

LIBC_SOURCES := $(SUPPORT_ROOT)/libc/ctype.cpp $(SUPPORT_ROOT)/libc/string.cpp
LIBC_OBJECTS := $(patsubst $(SUPPORT_ROOT)/%.cpp,$(BUILD_DIR)/support/%.o,$(LIBC_SOURCES))

SHARED_LIBRARY := $(BUILD_DIR)/$(SONAME)
LOCAL_SHARED_LIBRARY := $(LIBRARY_DIR)/$(SONAME)
LOCAL_LINKER_NAME := $(LIBRARY_DIR)/$(LINKER_NAME)

PACKAGE_ROOT := $(BUILD_DIR)/pkgroot
PACKAGE_ZIP := $(OUT_DIR)/$(PACKAGE).zip

COMMON_ARCH_FLAGS := -m64 -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -mno-avx -mno-avx512f
COMMON_FREESTANDING_FLAGS := -ffreestanding -fno-builtin -fno-stack-protector -nostdlib
CXXFLAGS ?= -std=c++20 -O2 $(COMMON_FREESTANDING_FLAGS) $(COMMON_ARCH_FLAGS) -fPIC \
            -fno-exceptions -fno-rtti -fno-gnu-unique -Wall -Wextra \
            -I$(INCLUDE_DIR) \
            -I$(SUPPORT_ROOT) \
            -I$(SUPPORT_ROOT)/crt \
            -I$(SUPPORT_ROOT)/helpers \
            -I$(SUPPORT_ROOT)/libc/include \
            -I$(NEUTRINO_ROOT)/shared/include
LDFLAGS ?= -shared -nostdlib -Wl,-soname,$(SONAME)

.PHONY: all shared package clean

all: shared

shared: $(LOCAL_SHARED_LIBRARY) $(LOCAL_LINKER_NAME)

package: shared $(PACKAGE_DIR)/manifest.toml | $(OUT_DIR)
	rm -rf $(PACKAGE_ROOT)
	mkdir -p $(PACKAGE_ROOT)/include/neutrino $(PACKAGE_ROOT)/library \
		$(PACKAGE_ROOT)/config $(PACKAGE_ROOT)/scripts
	cp $(INCLUDE_DIR)/neutrino/http.hpp $(PACKAGE_ROOT)/include/neutrino/http.hpp
	cp $(INCLUDE_DIR)/neutrino/net.hpp $(PACKAGE_ROOT)/include/neutrino/net.hpp
	cp $(LOCAL_SHARED_LIBRARY) $(PACKAGE_ROOT)/library/$(SONAME)
	cp $(LOCAL_LINKER_NAME) $(PACKAGE_ROOT)/library/$(LINKER_NAME)
	cp $(PACKAGE_DIR)/manifest.toml $(PACKAGE_ROOT)/manifest.toml
	rm -f $(PACKAGE_ZIP)
	cd $(PACKAGE_ROOT) && $(ZIP) -0 -r "$(abspath $(PACKAGE_ZIP))" \
		manifest.toml include library config scripts

$(BUILD_DIR)/src/%.o: src/%.cpp | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/support/%.o: $(SUPPORT_ROOT)/%.cpp | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(SHARED_LIBRARY): $(OBJECTS) $(LIBC_OBJECTS)
	$(CXX) $(LDFLAGS) $^ -o $@

$(LOCAL_SHARED_LIBRARY): $(SHARED_LIBRARY)
	@mkdir -p $(LIBRARY_DIR)
	cp $< $@

$(LOCAL_LINKER_NAME): $(SHARED_LIBRARY)
	@mkdir -p $(LIBRARY_DIR)
	cp $< $@

$(BUILD_DIR) $(OUT_DIR):
	mkdir -p $@

clean:
	rm -rf $(BUILD_DIR) $(OUT_DIR) $(LIBRARY_DIR)
