# BearSSL package build for Neutrino.
#
# This package provides the runtime shared library used by programs with
# DT_NEEDED=libbearssl.so.0, plus the development headers consumers need when
# building against this package out of tree.
#
# Expected package payload:
#   /include/bearssl*.h
#   /library/libbearssl.so.0
#   /library/libbearssl.so
#
# By default, the Makefile builds from the BearSSL sources in ./src and the
# public headers in ./include/bearssl.
#
# Local package source layout:
#   include/bearssl/bearssl.h
#   src/*.c
#
# Override BEARSSL_SRC if an upstream BearSSL tree lives elsewhere:
#   make BEARSSL_SRC=/path/to/BearSSL

SHELL := /bin/bash

PACKAGE ?= bearssl

CC  ?= gcc
CXX ?= g++
AR  ?= ar
ZIP ?= zip

BUILD_DIR ?= build
OUT_DIR ?= out
PACKAGE_DIR ?= package

BEARSSL_SRC ?=
ifneq ($(BEARSSL_SRC),)
BEARSSL_INC_DIR := $(BEARSSL_SRC)/inc
BEARSSL_SOURCE_DIR := $(BEARSSL_SRC)/src
else
BEARSSL_INC_DIR := include/bearssl
BEARSSL_SOURCE_DIR := src
endif
HAVE_BEARSSL_SOURCE := $(and $(wildcard $(BEARSSL_INC_DIR)/bearssl.h),$(wildcard $(BEARSSL_SOURCE_DIR)))

LIB_DIR := lib
LOCAL_LIBRARY_DIR := library
LOCAL_INCLUDE_DIR := include/bearssl

SONAME := libbearssl.so.0
LINKER_NAME := libbearssl.so

BEARSSL_HEADERS := $(sort $(wildcard $(BEARSSL_INC_DIR)/bearssl*.h))
BEARSSL_SOURCES := $(sort $(shell if [ -d "$(BEARSSL_SOURCE_DIR)" ]; then find "$(BEARSSL_SOURCE_DIR)" -type f -name '*.c'; fi))
BEARSSL_OBJECTS := $(patsubst $(BEARSSL_SOURCE_DIR)/%.c,$(BUILD_DIR)/bearssl/%.o,$(BEARSSL_SOURCES))

SUPPORT_SOURCES := $(LIB_DIR)/string.cpp
SUPPORT_OBJECTS := $(patsubst %.cpp,$(BUILD_DIR)/%.o,$(SUPPORT_SOURCES))

SHARED_LIBRARY := $(BUILD_DIR)/$(SONAME)
LOCAL_SHARED_LIBRARY := $(LOCAL_LIBRARY_DIR)/$(SONAME)
LOCAL_LINKER_NAME := $(LOCAL_LIBRARY_DIR)/$(LINKER_NAME)
ifneq ($(BEARSSL_SRC),)
LOCAL_HEADERS := $(addprefix $(LOCAL_INCLUDE_DIR)/,$(notdir $(BEARSSL_HEADERS)))
else
LOCAL_HEADERS :=
endif

PACKAGE_ROOT := $(BUILD_DIR)/pkgroot
PACKAGE_ZIP := $(OUT_DIR)/$(PACKAGE).zip

COMMON_ARCH_FLAGS := -m64 -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -mno-avx -mno-avx512f
COMMON_FREESTANDING_FLAGS := -ffreestanding -fno-builtin -fno-stack-protector -nostdlib

BEARSSL_CONFIG_FLAGS := \
	-DBR_RDRAND=0 \
	-DBR_USE_GETENTROPY=0 \
	-DBR_USE_URANDOM=0 \
	-DBR_USE_WIN32_RAND=0 \
	-DBR_USE_UNIX_TIME=0 \
	-DBR_USE_WIN32_TIME=0 \
	-DBR_AES_X86NI=0 \
	-DBR_SSE2=0 \
	-DBR_POWER8=0

CPPFLAGS += -I$(BEARSSL_INC_DIR) -I$(BEARSSL_SOURCE_DIR) $(BEARSSL_CONFIG_FLAGS)
CFLAGS += -std=c99 -O2 $(COMMON_FREESTANDING_FLAGS) $(COMMON_ARCH_FLAGS) -fPIC -Wall -Wextra
CXXFLAGS += -O2 $(COMMON_FREESTANDING_FLAGS) $(COMMON_ARCH_FLAGS) -fPIC -Wall -Wextra -Iinclude
LDFLAGS += -shared -nostdlib

.PHONY: all shared headers package clean check-source check-payload

ifeq ($(HAVE_BEARSSL_SOURCE),)
PACKAGE_PREREQS := check-payload
all: check-payload
shared: check-payload
else
PACKAGE_PREREQS := shared headers
all: shared headers
shared: $(LOCAL_SHARED_LIBRARY) $(LOCAL_LINKER_NAME) headers
endif

headers: check-source $(LOCAL_HEADERS)

package: $(PACKAGE_PREREQS) $(PACKAGE_DIR)/manifest.toml | $(OUT_DIR)
	rm -rf $(PACKAGE_ROOT)
	mkdir -p $(PACKAGE_ROOT)/include $(PACKAGE_ROOT)/library $(PACKAGE_ROOT)/scripts
	cp $(LOCAL_INCLUDE_DIR)/bearssl*.h $(PACKAGE_ROOT)/include/
	cp $(LOCAL_SHARED_LIBRARY) $(PACKAGE_ROOT)/library/$(SONAME)
	cp $(LOCAL_LINKER_NAME) $(PACKAGE_ROOT)/library/$(LINKER_NAME)
	cp $(PACKAGE_DIR)/manifest.toml $(PACKAGE_ROOT)/manifest.toml
	rm -f $(PACKAGE_ZIP)
	cd $(PACKAGE_ROOT) && $(ZIP) -0 -r "$(abspath $(PACKAGE_ZIP))" manifest.toml include library scripts

check-source:
	@if [[ ! -f "$(BEARSSL_INC_DIR)/bearssl.h" || ! -d "$(BEARSSL_SOURCE_DIR)" ]]; then \
		echo "BearSSL source tree not found at $(BEARSSL_SRC)." >&2; \
		echo "Expected $(BEARSSL_INC_DIR)/bearssl.h and $(BEARSSL_SOURCE_DIR)/." >&2; \
		echo "Vendor upstream BearSSL there, or run: make BEARSSL_SRC=/path/to/BearSSL" >&2; \
		false; \
	fi

check-payload:
	@if [[ ! -f "$(LOCAL_INCLUDE_DIR)/bearssl.h" ]]; then \
		echo "Missing staged BearSSL headers in $(LOCAL_INCLUDE_DIR)." >&2; \
		echo "Vendor upstream BearSSL into $(BEARSSL_SRC), or set BEARSSL_SRC=/path/to/BearSSL." >&2; \
		false; \
	fi
	@if [[ ! -f "$(LOCAL_SHARED_LIBRARY)" ]]; then \
		echo "Missing staged runtime library $(LOCAL_SHARED_LIBRARY)." >&2; \
		echo "Vendor upstream BearSSL into $(BEARSSL_SRC), or set BEARSSL_SRC=/path/to/BearSSL." >&2; \
		false; \
	fi

$(BUILD_DIR) $(OUT_DIR):
	mkdir -p $@

$(BUILD_DIR)/bearssl/%.o: $(BEARSSL_SOURCE_DIR)/%.c | check-source $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/lib/%.o: $(LIB_DIR)/%.cpp | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(SHARED_LIBRARY): $(BEARSSL_OBJECTS) $(SUPPORT_OBJECTS)
	@if [[ -z "$(BEARSSL_OBJECTS)" ]]; then \
		echo "No BearSSL source files found under $(BEARSSL_SOURCE_DIR)." >&2; \
		false; \
	fi
	$(CC) $(LDFLAGS) -Wl,-soname,$(SONAME) $^ -o $@

$(LOCAL_SHARED_LIBRARY): $(SHARED_LIBRARY)
	@mkdir -p $(LOCAL_LIBRARY_DIR)
	cp $< $@

$(LOCAL_LINKER_NAME): $(SHARED_LIBRARY)
	@mkdir -p $(LOCAL_LIBRARY_DIR)
	cp $< $@

$(LOCAL_INCLUDE_DIR)/%.h: $(BEARSSL_INC_DIR)/%.h | check-source
	@mkdir -p $(LOCAL_INCLUDE_DIR)
	cp $< $@

clean:
	rm -rf $(BUILD_DIR) $(OUT_DIR)
