PACKAGE := neutrino-live
BUILD_DIR ?= build
OUT_DIR ?= out
PACKAGE_ROOT := $(BUILD_DIR)/pkgroot
PACKAGE_ZIP := $(OUT_DIR)/$(PACKAGE).zip

.PHONY: package clean
package: package/manifest.toml
	rm -rf $(PACKAGE_ROOT)
	mkdir -p $(PACKAGE_ROOT)/config $(PACKAGE_ROOT)/scripts $(OUT_DIR)
	cp package/manifest.toml $(PACKAGE_ROOT)/manifest.toml
	rm -f $(PACKAGE_ZIP)
	cd $(PACKAGE_ROOT) && zip -0 -r "$(abspath $(PACKAGE_ZIP))" manifest.toml config scripts

clean:
	rm -rf $(BUILD_DIR) $(OUT_DIR)
