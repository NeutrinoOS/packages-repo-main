# Experimental headless OpenJDK 8 cross build for Neutrino.

SHELL := /bin/bash

PACKAGE := openjdk-8
NEUTRINO_ROOT ?= ../../neutrino
SOURCE_DIR ?= src
BUILD_DIR ?= build-neutrino-v2
PACKAGE_DIR ?= package
TOOLS_DIR := $(abspath tools)
NEUTRINO_SYSROOT := $(abspath $(NEUTRINO_ROOT)/userspace/build/newlib-pie-sysroot)
NEUTRINO_TARGET := x86_64-elf
TARGET_SYSROOT := $(NEUTRINO_SYSROOT)/$(NEUTRINO_TARGET)
BOOT_JDK ?= /usr/lib/jvm/java-8-openjdk

.PHONY: configure hotspot clean check-sdk

configure: check-sdk
	@chmod +x $(TOOLS_DIR)/neutrino-gcc $(TOOLS_DIR)/neutrino-g++
	@mkdir -p $(BUILD_DIR)
	cd $(BUILD_DIR) && \
		PATH=$(TOOLS_DIR):$$PATH \
		NEUTRINO_ROOT=$(abspath $(NEUTRINO_ROOT)) \
		CC=neutrino-gcc CXX=neutrino-g++ \
		AR=$(NEUTRINO_TARGET)-ar RANLIB=$(NEUTRINO_TARGET)-ranlib STRIP=$(NEUTRINO_TARGET)-strip \
		bash ../$(SOURCE_DIR)/configure \
			--openjdk-target=x86_64-unknown-freebsd \
			--disable-headful \
			--with-boot-jdk=$(BOOT_JDK) \
			--with-sys-root=$(TARGET_SYSROOT)

# HotSpot is the first native component to build.  It is intentionally split
# from configure because OpenJDK refuses to reconfigure a non-empty output
# directory; use BUILD_DIR=<fresh-directory> when changing configure options.
hotspot: check-sdk
	@test -f "$(BUILD_DIR)/spec.gmk" || { \
		echo "No OpenJDK configuration in $(BUILD_DIR); run: make configure" >&2; exit 1; \
	}
	$(MAKE) -C $(BUILD_DIR) JOBS=$${JOBS:-4} hotspot

check-sdk:
	@test -f "$(TARGET_SYSROOT)/lib/crt0.o" || { \
		echo "Neutrino Newlib SDK is missing; run: make -C $(NEUTRINO_ROOT)/userspace newlib-sdk" >&2; exit 1; \
	}
	@test -f "$(NEUTRINO_SYSROOT)/include/sys/socket.h" || { \
		echo "Neutrino socket headers are missing from the SDK; run: make -C $(NEUTRINO_ROOT)/userspace newlib-sdk" >&2; exit 1; \
	}
	@test -x "$(BOOT_JDK)/bin/java" || { \
		echo "A JDK 8 boot JDK is required at $(BOOT_JDK)" >&2; exit 1; \
	}

clean:
	@echo "OpenJDK build output is preserved at $(BUILD_DIR); remove it manually if a clean rebuild is needed."
