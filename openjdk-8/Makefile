# Experimental headless OpenJDK 8 cross build for Neutrino.

SHELL := /bin/bash

PACKAGE := openjdk-8
OUT_DIR ?= out
NEUTRINO_ROOT ?= ../../neutrino
SOURCE_DIR ?= src
BUILD_DIR ?= build
JDK_BUILD_DIR ?= build-neutrino-v2
PACKAGE_DIR ?= package
TOOLS_DIR := $(abspath tools)
NEUTRINO_SYSROOT := $(abspath $(NEUTRINO_ROOT)/userspace/build/newlib-pie-sysroot)
NEUTRINO_TARGET := x86_64-elf
TARGET_SYSROOT := $(NEUTRINO_SYSROOT)/$(NEUTRINO_TARGET)
BOOT_JDK ?= /usr/lib/jvm/java-8-openjdk

.PHONY: configure hotspot build image package clean check-sdk verify-package

configure: check-sdk
	@chmod +x $(TOOLS_DIR)/neutrino-gcc $(TOOLS_DIR)/neutrino-g++
	@mkdir -p $(JDK_BUILD_DIR)
	cd $(JDK_BUILD_DIR) && \
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
	@test -f "$(JDK_BUILD_DIR)/spec.gmk" || { \
		echo "No OpenJDK configuration in $(JDK_BUILD_DIR); run: make configure" >&2; exit 1; \
	}
	env -u MAKEFLAGS $(MAKE) -C $(JDK_BUILD_DIR) JOBS=$${JOBS:-4} hotspot

build: check-sdk
	@test -f "$(JDK_BUILD_DIR)/spec.gmk" || { \
		echo "No OpenJDK configuration in $(JDK_BUILD_DIR); run: make configure" >&2; exit 1; \
	}
	env -u MAKEFLAGS $(MAKE) -C $(JDK_BUILD_DIR) JOBS=$${JOBS:-4} jdk

# Use images-only rather than images: demos are deliberately not part of the
# headless server profile.  The source profile also omits optional Nashorn and
# zipfs jars, whose legacy host-side generators are unavailable.
image: build
	env -u MAKEFLAGS $(MAKE) -C $(JDK_BUILD_DIR) JOBS=$${JOBS:-4} images-only

package: image $(PACKAGE_DIR)/manifest.toml
	@rm -rf "$(BUILD_DIR)/pkgroot"
	@mkdir -p "$(BUILD_DIR)/pkgroot/binary" "$(BUILD_DIR)/pkgroot/library/$(PACKAGE)" \
		"$(BUILD_DIR)/pkgroot/config/$(PACKAGE)" "$(OUT_DIR)"
	@cp "$(JDK_BUILD_DIR)/images/j2re-image/bin/java" "$(BUILD_DIR)/pkgroot/binary/java"
	@cp -a "$(JDK_BUILD_DIR)/images/j2re-image/lib" \
		"$(BUILD_DIR)/pkgroot/config/$(PACKAGE)/"
	@find "$(JDK_BUILD_DIR)/jdk/lib/amd64" -maxdepth 1 -type f -name '*.so' \
		! -name 'libsaproc.so' \
		-exec cp {} "$(BUILD_DIR)/pkgroot/config/$(PACKAGE)/lib/amd64/" \;
	@cp "$(JDK_BUILD_DIR)/jdk/lib/amd64/jli/libjli.so" \
		"$(BUILD_DIR)/pkgroot/config/$(PACKAGE)/lib/amd64/jli/libjli.so"
	@cp "$(JDK_BUILD_DIR)/jdk/lib/amd64/server/libjvm.so" \
		"$(BUILD_DIR)/pkgroot/config/$(PACKAGE)/lib/amd64/server/libjvm.so"
	@cp "$(JDK_BUILD_DIR)/images/j2re-image/"LICENSE \
		"$(JDK_BUILD_DIR)/images/j2re-image/"ASSEMBLY_EXCEPTION \
		"$(JDK_BUILD_DIR)/images/j2re-image/"THIRD_PARTY_README \
		"$(JDK_BUILD_DIR)/images/j2re-image/"release \
		"$(BUILD_DIR)/pkgroot/config/$(PACKAGE)/"
	@find "$(JDK_BUILD_DIR)/jdk/lib/amd64" -type f -name '*.so' \
		! -name 'libsaproc.so' \
		-exec cp {} "$(BUILD_DIR)/pkgroot/library/$(PACKAGE)/" \;
	@cp "$(PACKAGE_DIR)/manifest.toml" "$(BUILD_DIR)/pkgroot/manifest.toml"
	@$(TOOLS_DIR)/audit-runtime-elf.sh "$(BUILD_DIR)/pkgroot"
	@rm -f "$(OUT_DIR)/$(PACKAGE).zip"
	@cd "$(BUILD_DIR)/pkgroot" && zip -0 -r "$(abspath $(OUT_DIR)/$(PACKAGE).zip)" \
		manifest.toml binary library config
	@$(MAKE) verify-package

verify-package:
	@test -f "$(OUT_DIR)/$(PACKAGE).zip"
	@unzip -Z1 "$(OUT_DIR)/$(PACKAGE).zip" | grep -qx 'manifest.toml'
	@unzip -Z1 "$(OUT_DIR)/$(PACKAGE).zip" | grep -qx 'binary/java'
	@unzip -Z1 "$(OUT_DIR)/$(PACKAGE).zip" | grep -qx 'library/$(PACKAGE)/libjvm.so'
	@unzip -Z1 "$(OUT_DIR)/$(PACKAGE).zip" | grep -qx 'library/$(PACKAGE)/libjava.so'
	@unzip -Z1 "$(OUT_DIR)/$(PACKAGE).zip" | grep -qx 'library/$(PACKAGE)/libnet.so'
	@unzip -Z1 "$(OUT_DIR)/$(PACKAGE).zip" | grep -qx 'config/$(PACKAGE)/lib/amd64/jvm.cfg'
	@unzip -Z1 "$(OUT_DIR)/$(PACKAGE).zip" | grep -qx 'config/$(PACKAGE)/lib/amd64/libzip.so'
	@unzip -Z1 "$(OUT_DIR)/$(PACKAGE).zip" | grep -qx 'config/$(PACKAGE)/lib/amd64/server/libjvm.so'
	@unzip -Z1 "$(OUT_DIR)/$(PACKAGE).zip" | grep -qx 'config/$(PACKAGE)/lib/rt.jar'
	@cmp "$(JDK_BUILD_DIR)/jdk/lib/amd64/server/libjvm.so" \
		"$(BUILD_DIR)/pkgroot/config/$(PACKAGE)/lib/amd64/server/libjvm.so"
	@$(TOOLS_DIR)/audit-runtime-elf.sh "$(BUILD_DIR)/pkgroot"

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
	@rm -rf "$(BUILD_DIR)" "$(OUT_DIR)"
	@echo "Configured OpenJDK output is preserved at $(JDK_BUILD_DIR)."
