#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Opens a Neutrino-native render context with one CPU-mappable BO.  Version 1
// accepts validated XRGB8888 fill operations only; it intentionally does not
// accept raw GPU command streams.
int neutrino_render_open(size_t bytes);
int neutrino_render_close(int context);
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

#ifdef __cplusplus
}
#endif
