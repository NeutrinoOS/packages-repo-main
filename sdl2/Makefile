SHELL := /bin/bash

PACKAGE := sdl2
VERSION := 0.1.5
NEUTRINO_ROOT ?= ../../neutrino
BUILD_DIR ?= build
OUT_DIR ?= out
STAGE_ROOT := $(OUT_DIR)/usr
SOURCE_DIR ?= src
PACKAGE_ROOT := $(BUILD_DIR)/pkgroot
NEUTRINO_CC := $(abspath $(NEUTRINO_ROOT)/userspace/newlib-port/neutrino-cc)

.PHONY: all configure compile verify package clean

all: compile

configure:
	NEUTRINO_CC=$(NEUTRINO_CC) cmake -S $(SOURCE_DIR) -B $(BUILD_DIR)/sdl2 \
		-DCMAKE_TOOLCHAIN_FILE=$(abspath cmake/neutrino-toolchain.cmake) \
		-DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/ \
		-DCMAKE_C_FLAGS_RELEASE="-O2 -DNDEBUG" \
		-DCMAKE_CXX_FLAGS_RELEASE="-O2 -DNDEBUG" \
		-DCMAKE_INSTALL_LIBDIR=library -DCMAKE_INSTALL_INCLUDEDIR=include \
		-DCMAKE_INSTALL_BINDIR=binary \
		-DSDL_SHARED=ON -DSDL_STATIC=ON -DSDL_TEST=OFF \
		-DSDL_LIBC=ON -DSDL_THREADS=ON -DSDL_PTHREADS=ON \
		-DSDL_AUDIO=ON -DSDL_VIDEO=ON -DSDL_RENDER=ON \
		-DSDL_JOYSTICK=OFF -DSDL_HAPTIC=OFF -DSDL_HIDAPI=OFF \
		-DSDL_SENSOR=OFF -DSDL_POWER=OFF -DSDL_FILESYSTEM=OFF \
		-DSDL_LOCALE=OFF -DSDL_MISC=OFF -DSDL_LOADSO=OFF \
		-DSDL_OPENGL=OFF -DSDL_OPENGLES=OFF -DSDL_VULKAN=OFF \
		-DSDL_X11=OFF -DSDL_WAYLAND=OFF -DSDL_KMSDRM=OFF \
		-DSDL_DUMMYAUDIO=OFF -DSDL_DUMMYVIDEO=OFF -DSDL_DISKAUDIO=OFF \
		-DSDL_OFFSCREEN=OFF -DSDL_LIBSAMPLERATE=OFF

compile: configure
	cmake --build $(BUILD_DIR)/sdl2 -j4
	rm -rf $(OUT_DIR)
	DESTDIR=$(abspath $(OUT_DIR)) cmake --install $(BUILD_DIR)/sdl2

verify: compile
	@test -f $(STAGE_ROOT)/library/libSDL2.so
	@x86_64-elf-readelf -h $(STAGE_ROOT)/library/libSDL2-2.0.so.0 | \
		awk '/Type:/ { if ($$2 != "DYN") exit 1 }'
	@x86_64-elf-readelf -d $(STAGE_ROOT)/library/libSDL2-2.0.so.0 | \
		grep -q 'Library soname: \[libSDL2-2.0.so.0\]'
	@for symbol in SDL_Init SDL_CreateWindow SDL_PollEvent SDL_OpenAudioDevice; do \
		x86_64-elf-nm -D --defined-only \
			$(STAGE_ROOT)/library/libSDL2-2.0.so.0 | \
			awk '{ print $$3 }' | grep -qx "$$symbol" || { \
				echo "Missing exported SDL2 symbol: $$symbol" >&2; exit 1; \
			}; \
	done
	@for symbol in NEUTRINO_bootstrap NEUTRINOAUDIO_bootstrap; do \
		x86_64-elf-nm --defined-only \
			$(STAGE_ROOT)/library/libSDL2-2.0.so.0 | \
			awk '{ print $$3 }' | grep -qx "$$symbol" || { \
				echo "Missing compiled Neutrino SDL2 driver: $$symbol" >&2; exit 1; \
			}; \
	done
	@bad_relocations="$$(x86_64-elf-readelf -rW \
		$(STAGE_ROOT)/library/libSDL2-2.0.so.0 | \
		awk '/R_X86_64_/ && $$3 != "R_X86_64_RELATIVE" && \
		     $$3 != "R_X86_64_64" && $$3 != "R_X86_64_GLOB_DAT" && \
		     $$3 != "R_X86_64_JUMP_SLOT" { print $$3 }' | sort -u)"; \
	if [[ -n "$$bad_relocations" ]]; then \
		echo "Unsupported SDL2 relocations: $$bad_relocations" >&2; exit 1; \
	fi

package: verify
	rm -rf $(PACKAGE_ROOT)
	mkdir -p $(PACKAGE_ROOT)/include $(PACKAGE_ROOT)/library $(PACKAGE_ROOT)/binary \
		$(PACKAGE_ROOT)/config $(PACKAGE_ROOT)/scripts
	cp -a $(STAGE_ROOT)/include/. $(PACKAGE_ROOT)/include/
	cp -a $(STAGE_ROOT)/library/. $(PACKAGE_ROOT)/library/
	cp $(STAGE_ROOT)/binary/sdl2-config $(PACKAGE_ROOT)/binary/sdl2-config
	sed -i 's#/usr/include#/include#g; s#/usr/library#/library#g' \
		$(PACKAGE_ROOT)/binary/sdl2-config \
		$(PACKAGE_ROOT)/library/pkgconfig/sdl2.pc \
		$(PACKAGE_ROOT)/library/cmake/SDL2/*.cmake
	cp package/manifest.toml $(PACKAGE_ROOT)/manifest.toml
	rm -f $(OUT_DIR)/$(PACKAGE).zip
	cd $(PACKAGE_ROOT) && zip -0 -r $(abspath $(OUT_DIR)/$(PACKAGE).zip) \
		manifest.toml binary config include library scripts

clean:
	rm -rf $(BUILD_DIR) $(OUT_DIR)
