PACKAGE := neutrino-drivers
NEUTRINO_ROOT ?= ../../neutrino
BUILD_DIR ?= build
OUT_DIR ?= out
PACKAGE_ROOT := $(BUILD_DIR)/pkgroot
PACKAGE_ZIP := $(OUT_DIR)/$(PACKAGE).zip
MODULES := $(NEUTRINO_ROOT)/out/modules/e1000e.ko
LOADS := $(NEUTRINO_ROOT)/out/modules/loads.txt

.PHONY: package clean
package: $(MODULES) $(LOADS) package/manifest.toml
	rm -rf $(PACKAGE_ROOT)
	mkdir -p $(PACKAGE_ROOT)/modules $(OUT_DIR)
	cp package/manifest.toml $(PACKAGE_ROOT)/manifest.toml
	cp $(MODULES) $(PACKAGE_ROOT)/modules/
	cp $(LOADS) $(PACKAGE_ROOT)/modules/loads.txt
	rm -f $(PACKAGE_ZIP)
	cd $(PACKAGE_ROOT) && zip -0 -r "$(abspath $(PACKAGE_ZIP))" manifest.toml modules

$(MODULES) $(LOADS):
	$(MAKE) -C $(NEUTRINO_ROOT) all

clean:
	rm -rf $(BUILD_DIR) $(OUT_DIR)
