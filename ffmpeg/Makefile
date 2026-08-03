# FFmpeg CLI and development-library package for Neutrino.

SHELL := /bin/bash

PACKAGE ?= ffmpeg
NEUTRINO_ROOT ?= ../../neutrino
NEUTRINO_TARGET ?= x86_64-elf
NEUTRINO_JOBS ?= 4
FFMPEG_THREAD_MODE ?= pthread

SOURCE_DIR ?= src
BUILD_DIR ?= build
OUT_DIR ?= out
PACKAGE_DIR ?= package

FFMPEG_BUILD_DIR := $(BUILD_DIR)/ffmpeg
FFMPEG_PROGRAMS := ffmpeg ffplay ffprobe
PROGRAM_TARGETS := $(addprefix $(OUT_DIR)/,$(addsuffix .elf,$(FFMPEG_PROGRAMS)))
FFMPEG_LIBRARY_DIRS := libavcodec libavdevice libavfilter libavformat \
                       libavutil libswresample libswscale
PACKAGE_ROOT := $(BUILD_DIR)/pkgroot
PACKAGE_ZIP := $(OUT_DIR)/$(PACKAGE).zip

NEUTRINO_USERSPACE := $(abspath $(NEUTRINO_ROOT)/userspace)
NEUTRINO_CONFIGURE := $(NEUTRINO_USERSPACE)/newlib-port/configure-ffmpeg.sh
SDL2_ROOT ?= ../sdl2/out/usr
SDL2_STAGE := $(abspath $(SDL2_ROOT))
SDL2_CONFIG := $(abspath ../sdl2/tools/sdl2-config-cross)
BEARSSL_ROOT ?= ../bearssl
BEARSSL_STAGE := $(abspath $(BEARSSL_ROOT))
NEUTRINO_SYSROOT := $(NEUTRINO_USERSPACE)/build/newlib-pie-sysroot
NEUTRINO_TARGET_ROOT := $(NEUTRINO_SYSROOT)/$(NEUTRINO_TARGET)
NEUTRINO_SDK_FILES := $(NEUTRINO_TARGET_ROOT)/lib/crt0.o \
                       $(NEUTRINO_TARGET_ROOT)/lib/libneutrino.a \
                       $(NEUTRINO_TARGET_ROOT)/lib/libc.a \
                       $(NEUTRINO_TARGET_ROOT)/lib/libm.a
READELF ?= $(NEUTRINO_TARGET)-readelf
NM ?= $(NEUTRINO_TARGET)-nm
ZIP ?= zip

.PHONY: all configure compile verify package clean check-source check-sdk

all: compile

configure: check-source check-sdk check-sdl check-bearssl | $(BUILD_DIR)
	SDL2_ROOT=$(SDL2_STAGE) SDL2_CONFIG=$(SDL2_CONFIG) \
		bash $(NEUTRINO_CONFIGURE) \
		$(abspath $(SOURCE_DIR)) $(abspath $(FFMPEG_BUILD_DIR)) \
		$(FFMPEG_THREAD_MODE)

compile: configure | $(OUT_DIR)
	$(MAKE) -C $(FFMPEG_BUILD_DIR) -j$(NEUTRINO_JOBS) $(FFMPEG_PROGRAMS)
	@for program in $(FFMPEG_PROGRAMS); do \
		cp "$(FFMPEG_BUILD_DIR)/$$program" "$(OUT_DIR)/$$program.elf"; \
	done
	rm -rf $(OUT_DIR)/include $(OUT_DIR)/library
	$(MAKE) -C $(FFMPEG_BUILD_DIR) install-headers \
		DESTDIR=$(abspath $(OUT_DIR))
	@for directory in $(FFMPEG_LIBRARY_DIRS); do \
		major="$$(find "$(FFMPEG_BUILD_DIR)/$$directory" -maxdepth 1 \
			-type f -name 'lib*.so.*' -print -quit)"; \
		if [[ -z "$$major" ]]; then \
			echo "Missing shared library for $$directory" >&2; \
			exit 1; \
		fi; \
		soname="$$(basename "$$major")"; \
		linker_name="$${soname%%.so.*}.so"; \
		install -m 0644 "$$major" "$(OUT_DIR)/library/$$soname"; \
		install -m 0644 "$$major" "$(OUT_DIR)/library/$$linker_name"; \
	done

verify: compile
	@for elf in $(PROGRAM_TARGETS); do \
		type="$$($(READELF) -h "$$elf" | awk '/Type:/ { print $$2 }')"; \
		if [[ "$$type" != DYN ]]; then \
			echo "Expected a PIE (ELF type DYN), got $$type for $$elf" >&2; \
			exit 1; \
		fi; \
	done
	@needed_count="$$($(READELF) -d $(OUT_DIR)/ffmpeg.elf | grep -c '(NEEDED)')"; \
	if [[ "$$needed_count" -ne 8 ]]; then \
		echo "Expected seven FFmpeg libraries plus BearSSL, got $$needed_count dependencies" >&2; \
		exit 1; \
	fi
	@for elf in $(PROGRAM_TARGETS) $(OUT_DIR)/library/*.so.*; do \
		bad_relocations="$$($(READELF) -rW "$$elf" | \
			awk '/R_X86_64_/ && $$3 != "R_X86_64_RELATIVE" && \
			     $$3 != "R_X86_64_64" && $$3 != "R_X86_64_GLOB_DAT" && \
			     $$3 != "R_X86_64_JUMP_SLOT" { print $$3 }' | sort -u)"; \
		if [[ -n "$$bad_relocations" ]]; then \
			echo "Unsupported relocations in $$elf: $$bad_relocations" >&2; \
			exit 1; \
		fi; \
	done
	@for elf in $(PROGRAM_TARGETS); do \
		$(READELF) -d "$$elf" | awk '/\(NEEDED\)/ { \
			name=$$NF; gsub(/^\[/, "", name); gsub(/\]$$/, "", name); print name \
		}' | while read -r needed; do \
			if [[ ! -f "$(OUT_DIR)/library/$$needed" && \
			      ! -f "$(SDL2_STAGE)/library/$$needed" && \
			      ! -f "$(BEARSSL_STAGE)/library/$$needed" ]]; then \
				echo "Missing packaged dependency $$needed for $$elf" >&2; \
				exit 1; \
			fi; \
		done; \
	done
	@definitions="$$(mktemp)"; undefined="$$(mktemp)"; \
	trap 'rm -f "$$definitions" "$$undefined"' EXIT; \
	for elf in $(PROGRAM_TARGETS) $(OUT_DIR)/library/*.so.* $(SDL2_STAGE)/library/*.so.* $(BEARSSL_STAGE)/library/*.so.*; do \
		$(NM) -D --defined-only "$$elf"; \
	done | awk '{ print $$3 }' | sed 's/@.*//' | sort -u > "$$definitions"; \
	for elf in $(PROGRAM_TARGETS) $(OUT_DIR)/library/*.so.* $(SDL2_STAGE)/library/*.so.* $(BEARSSL_STAGE)/library/*.so.*; do \
		$(NM) -D --undefined-only "$$elf"; \
	done | awk '{ print $$2 }' | sed 's/@.*//' | sort -u > "$$undefined"; \
	unresolved="$$(comm -23 "$$undefined" "$$definitions")"; \
	if [[ -n "$$unresolved" ]]; then \
		echo "Unresolved packaged symbols:" >&2; \
		echo "$$unresolved" >&2; \
		exit 1; \
	fi

package: verify $(PACKAGE_DIR)/manifest.toml | $(OUT_DIR)
	rm -rf $(PACKAGE_ROOT)
	mkdir -p $(PACKAGE_ROOT)/binary $(PACKAGE_ROOT)/config \
		$(PACKAGE_ROOT)/include $(PACKAGE_ROOT)/library $(PACKAGE_ROOT)/scripts
	for program in $(FFMPEG_PROGRAMS); do \
		cp "$(OUT_DIR)/$$program.elf" "$(PACKAGE_ROOT)/binary/$$program.elf"; \
	done
	cp -a $(OUT_DIR)/include/. $(PACKAGE_ROOT)/include/
	cp -a $(OUT_DIR)/library/. $(PACKAGE_ROOT)/library/
	cp $(PACKAGE_DIR)/manifest.toml $(PACKAGE_ROOT)/manifest.toml
	rm -f $(PACKAGE_ZIP)
	cd $(PACKAGE_ROOT) && $(ZIP) -0 -r "$(abspath $(PACKAGE_ZIP))" \
		manifest.toml binary config include library scripts

check-source:
	@if [[ ! -x "$(SOURCE_DIR)/configure" ]]; then \
		echo "FFmpeg source checkout not found under $(SOURCE_DIR)." >&2; \
		exit 1; \
	fi

check-sdk:
	@if [[ ! -f "$(NEUTRINO_CONFIGURE)" ]]; then \
		echo "Neutrino FFmpeg toolchain helper is missing: $(NEUTRINO_CONFIGURE)" >&2; \
		exit 1; \
	fi
	@for file in $(NEUTRINO_SDK_FILES); do \
		if [[ ! -f "$$file" ]]; then \
			echo "Neutrino Newlib SDK is missing: $$file" >&2; \
			echo "Run: make -C $(NEUTRINO_USERSPACE) newlib-sdk" >&2; \
			exit 1; \
		fi; \
	done

check-sdl:
	@test -f "$(SDL2_STAGE)/include/SDL2/SDL.h" || { \
		echo "Build the sibling sdl2 package first: make -C ../sdl2" >&2; exit 1; \
	}
	@test -f "$(SDL2_STAGE)/library/libSDL2.so" || { \
		echo "Missing staged Neutrino libSDL2.so" >&2; exit 1; \
	}
	@chmod +x $(SDL2_CONFIG)

check-bearssl:
	@test -f "$(BEARSSL_STAGE)/include/bearssl/bearssl.h" || { \
		echo "Missing sibling BearSSL headers" >&2; exit 1; \
	}
	@test -f "$(BEARSSL_STAGE)/library/libbearssl.so" || { \
		echo "Build the sibling bearssl package first: make -C ../bearssl" >&2; exit 1; \
	}

$(BUILD_DIR) $(OUT_DIR):
	mkdir -p $@


clean:
	rm -rf $(BUILD_DIR) $(OUT_DIR)
