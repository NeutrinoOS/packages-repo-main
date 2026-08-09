PACKAGE := base-system
PROGRAMS := init shell desktop service
PACKAGE_EXTRA_COMMANDS = cp $(OUT_DIR)/init.elf $(PACKAGE_ROOT)/init.elf
PACKAGE_EXTRA_PATHS = init.elf

include ../userspace-support/package.mk
