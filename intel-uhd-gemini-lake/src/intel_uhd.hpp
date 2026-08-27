#pragma once

namespace intel_uhd {

void configure();
void register_driver();
void init();

bool available();
bool blit_copy(unsigned int src_x,
               unsigned int src_y,
               unsigned int dst_x,
               unsigned int dst_y,
               unsigned int width,
               unsigned int height,
               unsigned int pitch_bytes);

// Registers this driver's scanout presenter with the kernel.  Presentation
// falls back to the CPU whenever the BLT engine or a surface is unsupported.
bool register_framebuffer_acceleration();

}  // namespace intel_uhd
