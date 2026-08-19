PACKAGE := desktop
PROGRAMS := desktop
PACKAGE_EXTRA_COMMANDS = mkdir -p $(PACKAGE_ROOT)/share/desktop/bitmaps && cp -a $(PACKAGE_DIR)/share/desktop/bitmaps/. $(PACKAGE_ROOT)/share/desktop/bitmaps/ && rm -f $(PACKAGE_ROOT)/share/desktop/bitmaps/.gitkeep
PACKAGE_EXTRA_PATHS = share

include ../userspace-support/package.mk

CXXFLAGS += -I../console-tools/include
CXXFLAGS += -I../userspace-support
