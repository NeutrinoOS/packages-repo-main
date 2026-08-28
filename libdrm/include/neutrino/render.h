#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Opens a Neutrino-native render context with one CPU-mappable BO.  Version 1
// accepts validated XRGB8888 fill operations only; it intentionally does not
// accept raw GPU command streams.
#define NEUTRINO_RENDER_ENGINE_BLT (1u << 0)
#define NEUTRINO_RENDER_ENGINE_RENDER (1u << 1)

#define NEUTRINO_RENDER_CAP_GPU_VA (1u << 0)
#define NEUTRINO_RENDER_CAP_EXECLISTS (1u << 1)
#define NEUTRINO_RENDER_CAP_PPGTT32 (1u << 2)
#define NEUTRINO_RENDER_CAP_3D_PIPELINE (1u << 3)
#define NEUTRINO_RENDER_CAP_STATE_BASE_ADDRESS (1u << 4)
#define NEUTRINO_RENDER_CAP_FRAGMENT_SHADER (1u << 5)
#define NEUTRINO_RENDER_CAP_RENDER_TARGET_WRITE (1u << 6)
#define NEUTRINO_RENDER_CAP_DEMO_DRAW (1u << 7)
#define NEUTRINO_RENDER_CAP_EXPLICIT_CACHE_SYNC (1u << 8)
#define NEUTRINO_RENDER_CAP_USER_SUBMISSION (1u << 31)

#define NEUTRINO_RENDER_SYNC_CPU_TO_DEVICE (1u << 0)
#define NEUTRINO_RENDER_SYNC_DEVICE_TO_CPU (1u << 1)

struct neutrino_render_device_info {
    uint16_t abi_major;
    uint16_t abi_minor;
    uint16_t vendor_id;
    uint16_t device_id;
    uint16_t graphics_version;
    uint16_t graphics_version_minor;
    uint32_t engines;
    uint32_t capabilities;
    uint32_t max_buffer_count;
    uint64_t max_buffer_bytes;
};

int neutrino_render_open(size_t bytes);
int neutrino_render_close(int context);
int neutrino_render_get_device_info(
    int context, struct neutrino_render_device_info* info);
void* neutrino_render_map(int context, size_t* bytes, int* gpu_accelerated);
int neutrino_render_create_bo(int context, uint32_t handle, size_t bytes);
int neutrino_render_destroy_bo(int context, uint32_t handle);
void* neutrino_render_map_bo(int context, uint32_t handle, size_t* bytes,
                             uint64_t* gpu_va);
int neutrino_render_fill(int context, uint64_t fence, uint64_t byte_offset,
                         uint32_t pitch_bytes, uint32_t x, uint32_t y,
                         uint32_t width, uint32_t height, uint32_t xrgb8888);
int neutrino_render_fill_bo(int context, uint32_t handle, uint64_t fence,
                            uint64_t byte_offset, uint32_t pitch_bytes,
                            uint32_t x, uint32_t y, uint32_t width,
                            uint32_t height, uint32_t xrgb8888);
uint64_t neutrino_render_completed_fence(int context);
int neutrino_render_fence_status(int context, uint64_t* submitted,
                                 uint64_t* completed, int* state,
                                 int* error);
int neutrino_render_wait_fence(int context, uint64_t fence,
                                uint64_t timeout_ticks);
int neutrino_render_draw_demo(int context, uint32_t handle, uint64_t fence);
int neutrino_render_sync_bo(int context, uint32_t handle, uint64_t byte_offset,
                            uint64_t byte_length, uint32_t flags);

#ifdef __cplusplus
}
#endif
