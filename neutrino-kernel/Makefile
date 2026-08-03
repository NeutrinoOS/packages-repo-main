# Package the kernel built by the sibling Neutrino source tree.

SHELL := /bin/bash

PACKAGE ?= neutrino-kernel
NEUTRINO_ROOT ?= ../../neutrino

OUT_DIR ?= out
BUILD_DIR ?= build
PACKAGE_DIR ?= package

KERNEL_ELF := $(NEUTRINO_ROOT)/out/kernel.elf
PACKAGE_ROOT := $(BUILD_DIR)/pkgroot
PACKAGE_ZIP := $(OUT_DIR)/$(PACKAGE).zip
KERNEL_VERSION := $(shell sed -nE 's/^version[[:space:]]*=[[:space:]]*"([^"]+)".*/\1/p' $(PACKAGE_DIR)/manifest.toml)

.PHONY: all kernel package clean

all: package

kernel: $(PACKAGE_DIR)/manifest.toml
	@if ! printf '%s\n' "$(KERNEL_VERSION)" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.-]+)?(\+[0-9A-Za-z.-]+)?$$'; then \
		echo "invalid kernel package version: $(KERNEL_VERSION)" >&2; \
		exit 1; \
	fi
	$(MAKE) -C $(NEUTRINO_ROOT) KERNEL_VERSION=$(KERNEL_VERSION) out/kernel.elf

package: kernel $(PACKAGE_DIR)/manifest.toml | $(OUT_DIR)
	rm -rf $(PACKAGE_ROOT)
	mkdir -p $(PACKAGE_ROOT)/boot
	cp $(KERNEL_ELF) $(PACKAGE_ROOT)/boot/kernel.elf
	cp $(PACKAGE_DIR)/manifest.toml $(PACKAGE_ROOT)/manifest.toml
	rm -f $(PACKAGE_ZIP)
	cd $(PACKAGE_ROOT) && zip -0 -r "$(abspath $(PACKAGE_ZIP))" manifest.toml boot

$(OUT_DIR):
	mkdir -p $@

clean:
	rm -rf $(BUILD_DIR) $(OUT_DIR)
