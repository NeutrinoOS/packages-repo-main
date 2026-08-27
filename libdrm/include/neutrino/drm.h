#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Neutrino descriptors are not Unix file descriptors. drmOpen returns the
// DRM descriptor handle directly, and this maps the single dumb buffer into
// the calling process without a separate mmap syscall.
void* neutrino_drm_map_dumb(int fd, size_t* size);
int neutrino_drm_present(int fd, uint32_t x, uint32_t y, uint32_t width, uint32_t height);
int neutrino_drm_is_accelerated(int fd);

#ifdef __cplusplus
}
#endif
