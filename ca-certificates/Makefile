PACKAGE := ca-certificates
BUILD_DIR ?= build
OUT_DIR ?= out
PACKAGE_DIR ?= package
PACKAGE_ROOT := $(BUILD_DIR)/pkgroot
PACKAGE_ZIP := $(OUT_DIR)/$(PACKAGE).zip
CA_BUNDLE := $(PACKAGE_DIR)/config/ssl/cacert.pem

.PHONY: package verify clean

package: verify $(PACKAGE_DIR)/manifest.toml
	rm -rf $(PACKAGE_ROOT)
	mkdir -p $(PACKAGE_ROOT)/config/ssl $(PACKAGE_ROOT)/scripts $(OUT_DIR)
	cp $(PACKAGE_DIR)/manifest.toml $(PACKAGE_ROOT)/manifest.toml
	cp $(PACKAGE_DIR)/config/ssl/cacert.pem $(PACKAGE_ROOT)/config/ssl/cacert.pem
	cp $(PACKAGE_DIR)/config/ssl/README.md $(PACKAGE_ROOT)/config/ssl/README.md
	rm -f $(PACKAGE_ZIP)
	cd $(PACKAGE_ROOT) && zip -0 -r "$(abspath $(PACKAGE_ZIP))" manifest.toml config scripts

verify: $(CA_BUNDLE)
	@test -s $(CA_BUNDLE) || { echo "CA certificate bundle is empty" >&2; exit 1; }
	@grep -q '^-----BEGIN CERTIFICATE-----$$' $(CA_BUNDLE) || { \
		echo "CA certificate bundle contains no PEM certificates" >&2; exit 1; \
	}

clean:
	rm -rf $(BUILD_DIR) $(OUT_DIR)
