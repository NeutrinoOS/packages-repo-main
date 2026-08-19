#pragma once

#include <stdint.h>

#define NEUTRINO_DESKTOP_REGISTRY_NAME "neutrino.desktop"
#define NEUTRINO_DESKTOP_REGISTRY_MAGIC 0x4e44534bu
#define NEUTRINO_DESKTOP_MESSAGE_MAGIC 0x4e44534du
#define NEUTRINO_DESKTOP_VERSION 1u
#define NEUTRINO_DESKTOP_PIXEL_ARGB8888 1u

enum {
    NEUTRINO_DESKTOP_CREATE = 1,
    NEUTRINO_DESKTOP_DAMAGE = 2,
    NEUTRINO_DESKTOP_DESTROY = 3,
    NEUTRINO_DESKTOP_CREATED = 4,
    NEUTRINO_DESKTOP_KEYBOARD = 5,
    NEUTRINO_DESKTOP_CLOSE = 6,
};

typedef struct {
    uint32_t magic;
    uint16_t version, reserved;
    uint32_t server_pipe_id, desktop_process_id;
} NeutrinoDesktopRegistry;

typedef struct {
    uint32_t magic;
    uint16_t version, type;
    uint32_t size, window_id;
    uint64_t token;
} NeutrinoDesktopMessageHeader;

typedef struct {
    NeutrinoDesktopMessageHeader header;
    uint32_t process_id, reply_pipe_id;
    uint32_t width, height, pixel_format, reserved;
    char surface_name[48], title[48];
} NeutrinoDesktopCreateMessage;

typedef struct {
    NeutrinoDesktopMessageHeader header;
    uint32_t x, y, width, height;
} NeutrinoDesktopDamageMessage;

typedef struct { NeutrinoDesktopMessageHeader header; }
    NeutrinoDesktopDestroyMessage;
typedef struct {
    NeutrinoDesktopMessageHeader header;
    int32_t status;
    uint32_t reserved;
} NeutrinoDesktopCreatedMessage;
typedef struct {
    NeutrinoDesktopMessageHeader header;
    uint16_t scancode, mods;
    uint32_t flags;
} NeutrinoDesktopKeyboardMessage;
typedef struct { NeutrinoDesktopMessageHeader header; }
    NeutrinoDesktopCloseMessage;
