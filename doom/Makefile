SHELL := /bin/bash

PROGRAM := doom
CC ?= gcc
CXX ?= g++
NASM ?= nasm

BUILD_DIR := build
OUT_DIR := out
PACKAGE_DIR := package
DOOM_DIR := doomgeneric

DOOM_SOURCES := \
	dummy.c am_map.c doomdef.c doomstat.c dstrings.c d_event.c d_items.c \
	d_iwad.c d_loop.c d_main.c d_mode.c d_net.c f_finale.c f_wipe.c \
	g_game.c hu_lib.c hu_stuff.c info.c i_cdmus.c i_endoom.c i_joystick.c \
	i_scale.c i_sound.c i_system.c i_timer.c memio.c m_argv.c m_bbox.c \
	m_cheat.c m_config.c m_controls.c m_fixed.c m_menu.c m_misc.c \
	m_random.c p_ceilng.c p_doors.c p_enemy.c p_floor.c p_inter.c \
	p_lights.c p_map.c p_maputl.c p_mobj.c p_plats.c p_pspr.c p_saveg.c \
	p_setup.c p_sight.c p_spec.c p_switch.c p_telept.c p_tick.c p_user.c \
	r_bsp.c r_data.c r_draw.c r_main.c r_plane.c r_segs.c r_sky.c \
	r_things.c sha1.c sounds.c statdump.c st_lib.c st_stuff.c s_sound.c \
	tables.c v_video.c wi_stuff.c w_checksum.c w_file.c w_file_stdc.c \
	w_main.c w_wad.c z_zone.c i_input.c i_video.c doomgeneric.c

CPP_SOURCES := lib/string.cpp lib/ctype.cpp lib/runtime.cpp \
	src/neutrino.cpp src/neutrino_sound.cpp
CRT_OBJECT := $(BUILD_DIR)/crt0.o
DOOM_OBJECTS := $(DOOM_SOURCES:%.c=$(BUILD_DIR)/doom/%.o)
CPP_OBJECTS := $(CPP_SOURCES:%.cpp=$(BUILD_DIR)/%.o)
OBJECTS := $(CRT_OBJECT) $(CPP_OBJECTS) $(DOOM_OBJECTS)

TARGET := $(OUT_DIR)/$(PROGRAM).elf
PACKAGE_ROOT := $(BUILD_DIR)/pkgroot
PACKAGE_ZIP := $(OUT_DIR)/$(PROGRAM).zip

COMMON_FLAGS := -ffreestanding -fno-builtin -fno-stack-protector -nostdlib \
	-m64 -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -mno-avx \
	-mno-avx512f -fPIE -O2 -ffunction-sections -fdata-sections \
	-fno-omit-frame-pointer -Wall -Wextra \
	-Iinclude -I$(DOOM_DIR) -DDOOMGENERIC_RESX=320 -DDOOMGENERIC_RESY=200 \
	-DFEATURE_NEUTRINO_SOUND
CFLAGS := $(COMMON_FLAGS) -std=gnu11 -Wno-unused-parameter \
	-Wno-missing-field-initializers
CXXFLAGS := $(COMMON_FLAGS) -std=c++20 -fno-exceptions -fno-rtti
LDFLAGS := -nostdlib -fPIE -pie -Wl,--gc-sections,--build-id=none

.PHONY: all package clean

all: $(TARGET)

$(TARGET): $(OBJECTS) | $(OUT_DIR)
	$(CXX) $(LDFLAGS) $(OBJECTS) -o $@

package: $(TARGET) $(PACKAGE_DIR)/manifest.toml | $(OUT_DIR)
	rm -rf $(PACKAGE_ROOT)
	mkdir -p $(PACKAGE_ROOT)/binary $(PACKAGE_ROOT)/config $(PACKAGE_ROOT)/scripts
	cp $(TARGET) $(PACKAGE_ROOT)/binary/$(PROGRAM).elf
	cp $(PACKAGE_DIR)/manifest.toml $(PACKAGE_ROOT)/manifest.toml
	rm -f $(PACKAGE_ZIP)
	cd $(PACKAGE_ROOT) && zip -0 -r "$(abspath $(PACKAGE_ZIP))" manifest.toml binary config scripts

$(BUILD_DIR)/crt0.o: crt/crt0.s | $(BUILD_DIR)
	$(NASM) -f elf64 $< -o $@

$(BUILD_DIR)/doom/%.o: $(DOOM_DIR)/%.c | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: %.cpp | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR) $(OUT_DIR):
	mkdir -p $@

clean:
	rm -rf $(BUILD_DIR) $(OUT_DIR)
