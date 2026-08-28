PACKAGE := intel-uhd-3d-demo
PROGRAMS := intel-uhd-3d-demo
EXTRA_LIBS := ../libdrm/library/libdrm.so.2

include ../userspace-support/package.mk

CXXFLAGS += -I../libdrm/include
CXXFLAGS += -I../userspace-support

$(PROGRAM_TARGETS): ../libdrm/library/libdrm.so.2

../libdrm/library/libdrm.so.2:
	$(MAKE) -C ../libdrm
