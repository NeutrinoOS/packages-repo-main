#include <neutrino/drm.h>
#include <neutrino/render.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <neutrino.h>

#include "syscall.hpp"

#include <stddef.h>
#include <stdint.h>

namespace {
constexpr uint32_t kRenderWidth = 64;
constexpr uint32_t kRenderHeight = 64;
constexpr uint32_t kRenderPitch = kRenderWidth * sizeof(uint32_t);
constexpr size_t kRenderBytes = kRenderPitch * kRenderHeight;
constexpr uint32_t kExpectedOrange = 0xffff4000u;
long g_console = -1;

int fail(const char* message, int render_context = -1, int drm = -1) {
    neutrino_write(g_console, "intel-uhd-3d-demo: ");
    neutrino_write_line(g_console, message);
    if (drm >= 0) (void)drmClose(drm);
    if (render_context >= 0) (void)neutrino_render_close(render_context);
    return 1;
}

void compose(uint32_t* display, uint32_t width, uint32_t height,
             uint32_t stride, const uint32_t* image) {
    for (uint32_t y = 0; y < height; ++y) {
        const uint32_t shade = 0x10u + (y * 0x20u / height);
        const uint32_t background =
            0xff000000u | (shade << 16) | (shade << 8) | (shade + 8u);
        for (uint32_t x = 0; x < width; ++x)
            display[static_cast<size_t>(y) * stride + x] = background;
    }

    uint32_t scale = width / kRenderWidth;
    const uint32_t vertical_scale = height / kRenderHeight;
    if (vertical_scale < scale) scale = vertical_scale;
    if (scale == 0) scale = 1;
    if (scale > 8) scale = 8;
    uint32_t shown_width = kRenderWidth * scale;
    uint32_t shown_height = kRenderHeight * scale;
    if (shown_width > width) shown_width = width;
    if (shown_height > height) shown_height = height;
    const uint32_t left = (width - shown_width) / 2;
    const uint32_t top = (height - shown_height) / 2;
    for (uint32_t y = 0; y < shown_height; ++y) {
        const uint32_t source_y = y / scale;
        for (uint32_t x = 0; x < shown_width; ++x) {
            display[static_cast<size_t>(top + y) * stride + left + x] =
                image[static_cast<size_t>(source_y) * kRenderWidth + x / scale];
        }
    }
}

// Returns null on success or the precise failed libdrm/KMS stage. Opening the
// DRM descriptor reserves a graphical-session framebuffer; SetCrtc activates
// it only when the kernel console is the current display owner.
const char* present_drm(const uint32_t* image) {
    const int drm = drmOpen(nullptr, nullptr);
    if (drm < 0) return "unable to acquire console DRM display lease";
    drmModeResPtr resources = drmModeGetResources(drm);
    if (resources == nullptr || resources->count_crtcs < 1 ||
        resources->count_connectors < 1) {
        (void)drmClose(drm);
        return "DRM display resources unavailable";
    }
    drmModeCrtcPtr crtc = drmModeGetCrtc(drm, resources->crtcs[0]);
    if (crtc == nullptr || crtc->width == 0 || crtc->height == 0) {
        (void)drmClose(drm);
        return "DRM console mode unavailable";
    }

    drm_mode_create_dumb dumb{};
    dumb.width = crtc->width;
    dumb.height = crtc->height;
    dumb.bpp = 32;
    if (drmModeCreateDumbBuffer(drm, &dumb) != 0 || dumb.pitch == 0) {
        (void)drmClose(drm);
        return "DRM framebuffer creation failed";
    }
    size_t display_bytes = 0;
    auto* display = static_cast<uint32_t*>(
        neutrino_drm_map_dumb(drm, &display_bytes));
    if (display == nullptr ||
        display_bytes < static_cast<size_t>(dumb.pitch) * dumb.height) {
        (void)drmClose(drm);
        return "DRM framebuffer mapping failed";
    }
    compose(display, dumb.width, dumb.height,
            dumb.pitch / sizeof(uint32_t), image);

    uint32_t framebuffer = 0;
    if (drmModeAddFB(drm, dumb.width, dumb.height, 24, 32, dumb.pitch,
                     dumb.handle, &framebuffer) != 0) {
        (void)drmClose(drm);
        return "DRM framebuffer registration failed";
    }
    uint32_t connector = resources->connectors[0];
    if (drmModeSetCrtc(drm, crtc->crtc_id, framebuffer, 0, 0,
                       &connector, 1, &crtc->mode) != 0) {
        (void)drmClose(drm);
        return "DRM modeset failed; run from the framebuffer console with no desktop active";
    }
    if (neutrino_drm_present(drm, 0, 0, dumb.width, dumb.height) != 0) {
        (void)drmClose(drm);
        return "DRM framebuffer presentation failed";
    }
    (void)sleep_ms(5000);
    (void)drmClose(drm);
    return nullptr;
}
}  // namespace

int main() {
    g_console = neutrino_open_stdout();
    const int render_context = neutrino_render_open(kRenderBytes);
    if (render_context < 0) return fail("unable to open render context");

    neutrino_render_device_info device{};
    if (neutrino_render_get_device_info(render_context, &device) != 0 ||
        device.abi_major != 1 || device.abi_minor < 5 ||
        (device.capabilities & NEUTRINO_RENDER_CAP_DEMO_DRAW) == 0 ||
        (device.capabilities &
         NEUTRINO_RENDER_CAP_EXPLICIT_CACHE_SYNC) == 0) {
        return fail("validated 3D demo draw is unavailable", render_context);
    }

    size_t render_bytes = 0;
    auto* image = static_cast<uint32_t*>(
        neutrino_render_map_bo(render_context, 1, &render_bytes, nullptr));
    if (image == nullptr || render_bytes < kRenderBytes)
        return fail("unable to map render BO", render_context);
    for (size_t i = 0; i < kRenderBytes / sizeof(uint32_t); ++i)
        image[i] = 0;
    if (neutrino_render_sync_bo(
            render_context, 1, 0, kRenderBytes,
            NEUTRINO_RENDER_SYNC_CPU_TO_DEVICE) != 0)
        return fail("CPU-to-GPU BO synchronization failed", render_context);

    constexpr uint64_t kFence = 1;
    if (neutrino_render_draw_demo(render_context, 1, kFence) != 0 ||
        neutrino_render_wait_fence(render_context, kFence, 0) != 0) {
        return fail("RCS demo request failed", render_context);
    }
    if (neutrino_render_sync_bo(
            render_context, 1, 0, kRenderBytes,
            NEUTRINO_RENDER_SYNC_DEVICE_TO_CPU) != 0)
        return fail("GPU-to-CPU BO synchronization failed", render_context);
    size_t orange_pixels = 0;
    for (size_t i = 0; i < kRenderBytes / sizeof(uint32_t); ++i)
        if (image[i] == kExpectedOrange) ++orange_pixels;
    if (orange_pixels == 0)
        return fail("RCS completed without the expected pixels", render_context);

    const char* present_error = present_drm(image);
    if (present_error != nullptr) return fail(present_error, render_context);
    neutrino_write_line(
        g_console,
        "intel-uhd-3d-demo: GPU-rendered image displayed successfully");
    (void)neutrino_render_close(render_context);
    return 0;
}
