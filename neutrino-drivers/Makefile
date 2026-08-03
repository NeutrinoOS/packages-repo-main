PACKAGE := neutrino-drivers
BUILD_DIR ?= build
OUT_DIR ?= out
PACKAGE_ROOT := $(BUILD_DIR)/pkgroot
PACKAGE_ZIP := $(OUT_DIR)/$(PACKAGE).zip

.PHONY: package clean
package: package/manifest.toml
	rm -rf $(PACKAGE_ROOT)
	mkdir -p $(PACKAGE_ROOT) $(OUT_DIR)
	cp package/manifest.toml $(PACKAGE_ROOT)/manifest.toml
	rm -f $(PACKAGE_ZIP)
	cd $(PACKAGE_ROOT) && zip -0 -r "$(abspath $(PACKAGE_ZIP))" manifest.toml

clean:
	rm -rf $(BUILD_DIR) $(OUT_DIR)
