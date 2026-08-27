SHELL := /bin/bash
PACKAGE := libdrm
SONAME := libdrm.so.2
VERSION := 1.0.0
NEUTRINO_ROOT ?= ../../neutrino
SUPPORT_ROOT ?= ../userspace-support
BUILD_DIR ?= build
OUT_DIR ?= out
INCLUDE_DIR := include
LIBRARY_DIR := library
PACKAGE_ROOT := $(BUILD_DIR)/pkgroot

CXX ?= g++
CXXFLAGS := -std=c++20 -O2 -ffreestanding -fno-builtin -fno-stack-protector -nostdlib \
	-m64 -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -mno-avx -mno-avx512f -fPIC \
	-fno-exceptions -fno-rtti -fno-gnu-unique -Wall -Wextra -I$(INCLUDE_DIR) \
	-I$(SUPPORT_ROOT) -I$(SUPPORT_ROOT)/crt -I$(SUPPORT_ROOT)/libc/include \
	-I$(NEUTRINO_ROOT)/shared/include

.PHONY: all package clean
all: $(LIBRARY_DIR)/$(SONAME) $(LIBRARY_DIR)/libdrm.so

$(BUILD_DIR)/drm.o: src/drm.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/$(SONAME): $(BUILD_DIR)/drm.o
	$(CXX) -shared -nostdlib -Wl,-soname,$(SONAME) $< -o $@

$(LIBRARY_DIR)/$(SONAME): $(BUILD_DIR)/$(SONAME)
	@mkdir -p $(LIBRARY_DIR)
	cp $< $@

$(LIBRARY_DIR)/libdrm.so: $(LIBRARY_DIR)/$(SONAME)
	cp $< $@

package: all package/manifest.toml | $(OUT_DIR)
	rm -rf $(PACKAGE_ROOT)
	mkdir -p $(PACKAGE_ROOT)/include/neutrino $(PACKAGE_ROOT)/library $(PACKAGE_ROOT)/config $(PACKAGE_ROOT)/scripts
	cp $(INCLUDE_DIR)/xf86drm.h $(PACKAGE_ROOT)/include/xf86drm.h
	cp $(INCLUDE_DIR)/xf86drmMode.h $(PACKAGE_ROOT)/include/xf86drmMode.h
	cp $(INCLUDE_DIR)/neutrino/drm.h $(PACKAGE_ROOT)/include/neutrino/drm.h
	cp $(INCLUDE_DIR)/neutrino/render.h $(PACKAGE_ROOT)/include/neutrino/render.h
	cp $(NEUTRINO_ROOT)/shared/include/neutrino_drm.hpp $(PACKAGE_ROOT)/include/neutrino/drm_abi.hpp
	cp $(NEUTRINO_ROOT)/shared/include/neutrino_render.hpp $(PACKAGE_ROOT)/include/neutrino/render_abi.hpp
	cp $(LIBRARY_DIR)/$(SONAME) $(PACKAGE_ROOT)/library/$(SONAME)
	cp $(LIBRARY_DIR)/libdrm.so $(PACKAGE_ROOT)/library/libdrm.so
	cp package/manifest.toml $(PACKAGE_ROOT)/manifest.toml
	rm -f $(OUT_DIR)/$(PACKAGE).zip
	cd $(PACKAGE_ROOT) && zip -0 -r "$(abspath $(OUT_DIR)/$(PACKAGE).zip)" manifest.toml include library config scripts

$(BUILD_DIR) $(OUT_DIR):
	mkdir -p $@

clean:
	rm -rf $(BUILD_DIR) $(OUT_DIR) $(LIBRARY_DIR)
