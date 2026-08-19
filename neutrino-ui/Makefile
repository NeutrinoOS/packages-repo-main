SHELL := /bin/bash
PACKAGE := neutrino-ui
PACKAGE_ROOT := build/pkgroot
OUT_DIR := out
.PHONY: all package clean
all:
	@true
package: package/manifest.toml ../userspace-support/ui.hpp ../userspace-support/window.hpp ../userspace-support/compositor.hpp ../userspace-support/desktop_protocol.hpp ../userspace-support/desktop_protocol.h
	rm -rf $(PACKAGE_ROOT)
	mkdir -p $(PACKAGE_ROOT)/include/neutrino $(PACKAGE_ROOT)/config $(PACKAGE_ROOT)/scripts $(OUT_DIR)
	cp ../userspace-support/ui.hpp $(PACKAGE_ROOT)/include/neutrino/ui.hpp
	cp ../userspace-support/window.hpp $(PACKAGE_ROOT)/include/neutrino/window.hpp
	cp ../userspace-support/compositor.hpp $(PACKAGE_ROOT)/include/neutrino/compositor.hpp
	cp ../userspace-support/desktop_protocol.hpp $(PACKAGE_ROOT)/include/neutrino/desktop_protocol.hpp
	cp ../userspace-support/desktop_protocol.h $(PACKAGE_ROOT)/include/neutrino/desktop_protocol.h
	cp package/manifest.toml $(PACKAGE_ROOT)/manifest.toml
	rm -f $(OUT_DIR)/$(PACKAGE).zip
	cd $(PACKAGE_ROOT) && zip -0 -r "$(abspath $(OUT_DIR)/$(PACKAGE).zip)" manifest.toml include config scripts
clean:
	rm -rf build $(OUT_DIR)
