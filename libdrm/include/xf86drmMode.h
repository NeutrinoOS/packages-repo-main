#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DRM_MODE_CONNECTED 1
#define DRM_MODE_TYPE_PREFERRED (1u << 3)
#define DRM_FORMAT_XRGB8888 0x34325258u

typedef struct _drmModeModeInfo {
    uint32_t clock;
    uint16_t hdisplay, hsync_start, hsync_end, htotal, hskew;
    uint16_t vdisplay, vsync_start, vsync_end, vtotal, vscan;
    uint32_t vrefresh;
    uint32_t flags;
    uint32_t type;
    char name[32];
} drmModeModeInfo;

typedef struct _drmModeRes {
    int count_fbs; uint32_t* fbs;
    int count_crtcs; uint32_t* crtcs;
    int count_connectors; uint32_t* connectors;
    int count_encoders; uint32_t* encoders;
    uint32_t min_width, max_width, min_height, max_height;
} drmModeRes, *drmModeResPtr;

typedef struct _drmModeConnector {
    uint32_t connector_id, encoder_id, connector_type, connector_type_id;
    uint32_t connection, mmWidth, mmHeight, subpixel;
    int count_modes; drmModeModeInfo* modes;
    int count_props; uint32_t* props; uint64_t* prop_values;
    int count_encoders; uint32_t* encoders;
} drmModeConnector, *drmModeConnectorPtr;

typedef struct _drmModeCrtc {
    uint32_t crtc_id, buffer_id;
    uint32_t x, y, width, height;
    int mode_valid;
    drmModeModeInfo mode;
    int gamma_size;
} drmModeCrtc, *drmModeCrtcPtr;

struct drm_mode_create_dumb { uint32_t height, width, bpp, flags, handle, pitch; uint64_t size; };
struct drm_mode_map_dumb { uint32_t handle, pad; uint64_t offset; };

drmModeResPtr drmModeGetResources(int fd);
void drmModeFreeResources(drmModeResPtr ptr);
drmModeConnectorPtr drmModeGetConnector(int fd, uint32_t connector_id);
void drmModeFreeConnector(drmModeConnectorPtr ptr);
drmModeCrtcPtr drmModeGetCrtc(int fd, uint32_t crtc_id);
void drmModeFreeCrtc(drmModeCrtcPtr ptr);
int drmModeSetCrtc(int fd, uint32_t crtc_id, uint32_t buffer_id, uint32_t x, uint32_t y,
                   uint32_t* connectors, int count, drmModeModeInfo* mode);
int drmModeCreateDumbBuffer(int fd, struct drm_mode_create_dumb* create);
int drmModeMapDumbBuffer(int fd, struct drm_mode_map_dumb* map);
int drmModeAddFB(int fd, uint32_t width, uint32_t height, uint8_t depth, uint8_t bpp,
                 uint32_t pitch, uint32_t handle, uint32_t* buffer_id);

#ifdef __cplusplus
}
#endif
