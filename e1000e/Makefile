PACKAGE := e1000e
MODULE := e1000e.ko
NEUTRINO_ROOT ?= ../../neutrino
CROSS ?= x86_64-elf-
CXX := $(CROSS)g++
BUILD_DIR ?= build
OUT_DIR ?= out
PACKAGE_ROOT := $(BUILD_DIR)/pkgroot
PACKAGE_ZIP := $(OUT_DIR)/$(PACKAGE).zip
MODULE_SOURCE := src/e1000e.cpp
MODULE_HEADER := src/e1000e.hpp
MODULE_OUTPUT := $(OUT_DIR)/$(MODULE)
MODULE_CXXFLAGS := -std=c++20 -g -ffreestanding -O2 -Wall -Wextra -m64 \
	-mno-red-zone -mno-sse -mno-mmx -mno-avx -mno-avx512f -mno-sse2 \
	-fno-exceptions -fno-rtti -mcmodel=kernel \
	-I$(NEUTRINO_ROOT)/shared/include -I$(NEUTRINO_ROOT)/src \
	-I$(NEUTRINO_ROOT)/src/arch/x86_64 \
	-I$(NEUTRINO_ROOT)/src/third_party/uacpi/include -Isrc \
	-DNEUTRINO_DYNAMIC_MODULE_E1000E $(EXTRA_CXXFLAGS)

.PHONY: package clean
package: $(MODULE_OUTPUT) package/manifest.toml
	rm -rf $(PACKAGE_ROOT)
	mkdir -p $(PACKAGE_ROOT)/modules $(OUT_DIR)
	cp package/manifest.toml $(PACKAGE_ROOT)/manifest.toml
	cp $(MODULE_OUTPUT) $(PACKAGE_ROOT)/modules/$(MODULE)
	rm -f $(PACKAGE_ZIP)
	cd $(PACKAGE_ROOT) && zip -0 -r "$(abspath $(PACKAGE_ZIP))" manifest.toml modules

$(MODULE_OUTPUT): $(MODULE_SOURCE) $(MODULE_HEADER) $(NEUTRINO_ROOT)/src/kernel/module.hpp
	@mkdir -p $(OUT_DIR)
	$(CXX) $(MODULE_CXXFLAGS) -c $(MODULE_SOURCE) -o $@

clean:
	rm -rf $(BUILD_DIR) $(OUT_DIR)
