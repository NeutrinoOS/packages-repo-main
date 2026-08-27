#include <neutrino/drm.h>
#include <neutrino/render.h>
#include <neutrino_drm.hpp>
#include <neutrino_render.hpp>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <stddef.h>
#include <stdint.h>

#include "crt/syscall.hpp"

namespace {

bool device_info(int fd, neutrino_drm::DeviceInfo& info) {
    return fd >= 0 && descriptor_get_property(static_cast<uint32_t>(fd),
        static_cast<uint32_t>(neutrino_drm::Property::DeviceInfo), &info, sizeof(info)) == 0 &&
        info.abi_major == 1;
}

bool mode_info(int fd, neutrino_drm::ModeInfo& mode) {
    return fd >= 0 && descriptor_get_property(static_cast<uint32_t>(fd),
        static_cast<uint32_t>(neutrino_drm::Property::ModeInfo), &mode, sizeof(mode)) == 0;
}

void mode_to_drm(const neutrino_drm::ModeInfo& source, drmModeModeInfo& target) {
    target = {};
    target.hdisplay = static_cast<uint16_t>(source.width);
    target.hsync_start = target.hdisplay;
    target.hsync_end = target.hdisplay;
    target.htotal = target.hdisplay;
    target.vdisplay = static_cast<uint16_t>(source.height);
    target.vsync_start = target.vdisplay;
    target.vsync_end = target.vdisplay;
    target.vtotal = target.vdisplay;
    target.vrefresh = source.vrefresh_millihz / 1000;
    target.type = DRM_MODE_TYPE_PREFERRED;
    const char prefix[] = "Neutrino";
    for (size_t i = 0; i < sizeof(prefix); ++i) target.name[i] = prefix[i];
}

}  // namespace

extern "C" int drmOpen(const char*, const char*) {
    long handle = descriptor_open(neutrino_drm::kDescriptorType);
    return handle < 0 ? -1 : static_cast<int>(handle);
}

extern "C" int drmClose(int fd) {
    return fd < 0 ? -1 : (descriptor_close(static_cast<uint32_t>(fd)) == 0 ? 0 : -1);
}

extern "C" int drmGetCap(int fd, uint64_t capability, uint64_t* value) {
    if (value == nullptr || capability != DRM_CAP_DUMB_BUFFER) return -1;
    neutrino_drm::DeviceInfo info{};
    if (!device_info(fd, info)) return -1;
    *value = 1;
    return 0;
}

extern "C" drmVersionPtr drmGetVersion(int) { return nullptr; }
extern "C" void drmFreeVersion(drmVersionPtr) {}

extern "C" drmModeResPtr drmModeGetResources(int fd) {
    neutrino_drm::ModeInfo mode{};
    if (!mode_info(fd, mode)) return nullptr;
    static uint32_t fbs[] = {neutrino_drm::kFramebufferId};
    static uint32_t crtcs[] = {neutrino_drm::kCrtcId};
    static uint32_t connectors[] = {neutrino_drm::kConnectorId};
    static drmModeRes result{};
    result = {1, fbs, 1, crtcs, 1, connectors, 0, nullptr, mode.width, mode.width, mode.height, mode.height};
    return &result;
}
extern "C" void drmModeFreeResources(drmModeResPtr) {}

extern "C" drmModeConnectorPtr drmModeGetConnector(int fd, uint32_t id) {
    neutrino_drm::ModeInfo mode{};
    if (id != neutrino_drm::kConnectorId || !mode_info(fd, mode)) return nullptr;
    static drmModeModeInfo drm_mode{};
    static uint32_t encoders[] = {1};
    static drmModeConnector connector{};
    mode_to_drm(mode, drm_mode);
    connector = {id, 1, 0, 0, DRM_MODE_CONNECTED, 0, 0, 0, 1, &drm_mode, 0, nullptr, nullptr, 1, encoders};
    return &connector;
}
extern "C" void drmModeFreeConnector(drmModeConnectorPtr) {}

extern "C" drmModeCrtcPtr drmModeGetCrtc(int fd, uint32_t id) {
    neutrino_drm::ModeInfo mode{};
    if (id != neutrino_drm::kCrtcId || !mode_info(fd, mode)) return nullptr;
    static drmModeCrtc crtc{};
    mode_to_drm(mode, crtc.mode);
    crtc.crtc_id = id; crtc.buffer_id = neutrino_drm::kFramebufferId;
    crtc.width = mode.width; crtc.height = mode.height; crtc.mode_valid = 1;
    return &crtc;
}
extern "C" void drmModeFreeCrtc(drmModeCrtcPtr) {}

extern "C" int drmModeSetCrtc(int fd, uint32_t crtc, uint32_t buffer, uint32_t x, uint32_t y,
                                uint32_t* connectors, int count, drmModeModeInfo* mode) {
    neutrino_drm::ModeInfo current{};
    if (x != 0 || y != 0 || connectors == nullptr || count != 1 ||
        connectors[0] != neutrino_drm::kConnectorId || mode == nullptr || !mode_info(fd, current) ||
        mode->hdisplay != current.width || mode->vdisplay != current.height) return -1;
    neutrino_drm::SetCrtc set{crtc, buffer, neutrino_drm::kConnectorId, 0, current};
    return descriptor_set_property(static_cast<uint32_t>(fd),
        static_cast<uint32_t>(neutrino_drm::Property::SetCrtc), &set, sizeof(set)) == 0 ? 0 : -1;
}

extern "C" int drmModeCreateDumbBuffer(int fd, struct drm_mode_create_dumb* create) {
    neutrino_drm::ModeInfo mode{};
    if (create == nullptr || !mode_info(fd, mode) || create->width != mode.width ||
        create->height != mode.height || create->bpp != 32 || create->flags != 0) return -1;
    create->handle = neutrino_drm::kDumbHandle; create->pitch = mode.pitch;
    create->size = static_cast<uint64_t>(mode.pitch) * mode.height;
    return 0;
}

extern "C" int drmModeMapDumbBuffer(int fd, struct drm_mode_map_dumb* map) {
    if (map == nullptr || map->handle != neutrino_drm::kDumbHandle) return -1;
    void* address = neutrino_drm_map_dumb(fd, nullptr);
    if (address == nullptr) return -1;
    map->offset = reinterpret_cast<uint64_t>(address);
    return 0;
}

extern "C" int drmModeAddFB(int fd, uint32_t width, uint32_t height, uint8_t depth, uint8_t bpp,
                              uint32_t pitch, uint32_t handle, uint32_t* buffer_id) {
    neutrino_drm::ModeInfo mode{};
    if (buffer_id == nullptr || !mode_info(fd, mode) || width != mode.width || height != mode.height ||
        depth != 24 || bpp != 32 || pitch != mode.pitch || handle != neutrino_drm::kDumbHandle) return -1;
    *buffer_id = neutrino_drm::kFramebufferId;
    return 0;
}

extern "C" void* neutrino_drm_map_dumb(int fd, size_t* size) {
    descriptor_defs::FramebufferInfo framebuffer{};
    if (fd < 0 || descriptor_get_property(static_cast<uint32_t>(fd),
        static_cast<uint32_t>(descriptor_defs::Property::FramebufferInfo), &framebuffer, sizeof(framebuffer)) != 0 ||
        framebuffer.virtual_base == 0) return nullptr;
    if (size != nullptr) *size = static_cast<size_t>(framebuffer.pitch) * framebuffer.height;
    return reinterpret_cast<void*>(framebuffer.virtual_base);
}

extern "C" int neutrino_drm_present(int fd, uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
    const neutrino_drm::Rect damage{x, y, width, height};
    return fd >= 0 && descriptor_set_property(static_cast<uint32_t>(fd),
        static_cast<uint32_t>(neutrino_drm::Property::Present), &damage, sizeof(damage)) == 0 ? 0 : -1;
}

extern "C" int neutrino_drm_is_accelerated(int fd) {
    neutrino_drm::DeviceInfo info{};
    return device_info(fd, info) && (info.flags & neutrino_drm::kDeviceGpuPresent) != 0;
}

extern "C" int neutrino_render_open(size_t bytes) {
    long context = descriptor_open(neutrino_render::kDescriptorType,
                                   static_cast<uint64_t>(bytes));
    return context < 0 ? -1 : static_cast<int>(context);
}

extern "C" int neutrino_render_close(int context) {
    return context < 0 ? -1 :
        (descriptor_close(static_cast<uint32_t>(context)) == 0 ? 0 : -1);
}

extern "C" void* neutrino_render_map(int context, size_t* bytes, int* gpu_accelerated) {
    neutrino_render::Info info{};
    if (context < 0 || descriptor_get_property(static_cast<uint32_t>(context),
        static_cast<uint32_t>(neutrino_render::Property::Info), &info, sizeof(info)) != 0 ||
        info.abi_major != 1 || info.virtual_base == 0) return nullptr;
    if (bytes != nullptr) *bytes = static_cast<size_t>(info.byte_length);
    if (gpu_accelerated != nullptr)
        *gpu_accelerated = (info.flags & neutrino_render::kInfoGpuBlt) != 0;
    return reinterpret_cast<void*>(info.virtual_base);
}

extern "C" int neutrino_render_fill(int context, uint64_t fence, uint64_t byte_offset,
                                     uint32_t pitch, uint32_t x, uint32_t y,
                                     uint32_t width, uint32_t height, uint32_t color) {
    const neutrino_render::Fill fill{fence, byte_offset, pitch, x, y, width, height, color, 0};
    return context < 0 ? -1 :
        (descriptor_set_property(static_cast<uint32_t>(context),
            static_cast<uint32_t>(neutrino_render::Property::SubmitFill),
            &fill, sizeof(fill)) == 0 ? 0 : -1);
}

extern "C" int neutrino_render_fill_bo(int context, uint32_t handle, uint64_t fence,
                                        uint64_t byte_offset, uint32_t pitch, uint32_t x,
                                        uint32_t y, uint32_t width, uint32_t height, uint32_t color) {
    const neutrino_render::Fill2 fill{handle, 0, {fence, byte_offset, pitch, x, y, width, height, color, 0}};
    return context < 0 ? -1 : (descriptor_set_property(static_cast<uint32_t>(context),
        static_cast<uint32_t>(neutrino_render::Property::SubmitFill2), &fill, sizeof(fill)) == 0 ? 0 : -1);
}

extern "C" uint64_t neutrino_render_completed_fence(int context) {
    neutrino_render::Info info{};
    return context < 0 || descriptor_get_property(static_cast<uint32_t>(context),
        static_cast<uint32_t>(neutrino_render::Property::Info), &info, sizeof(info)) != 0
        ? 0 : info.completed_fence;
}

extern "C" int neutrino_render_create_bo(int context, uint32_t handle, size_t bytes) {
    const neutrino_render::BufferCreate request{handle, 0, static_cast<uint64_t>(bytes)};
    return context < 0 ? -1 : (descriptor_set_property(static_cast<uint32_t>(context),
        static_cast<uint32_t>(neutrino_render::Property::CreateBuffer), &request, sizeof(request)) == 0 ? 0 : -1);
}

extern "C" int neutrino_render_destroy_bo(int context, uint32_t handle) {
    const neutrino_render::BufferDestroy request{handle, 0};
    return context < 0 ? -1 : (descriptor_set_property(static_cast<uint32_t>(context),
        static_cast<uint32_t>(neutrino_render::Property::DestroyBuffer), &request, sizeof(request)) == 0 ? 0 : -1);
}

extern "C" void* neutrino_render_map_bo(int context, uint32_t handle, size_t* bytes, uint64_t* gpu_va) {
    if (context < 0 || handle == 0 || handle > 4) return nullptr;
    neutrino_render::BufferInfo info{};
    uint32_t property = static_cast<uint32_t>(neutrino_render::Property::BufferInfo0) + handle - 1;
    if (descriptor_get_property(static_cast<uint32_t>(context), property, &info, sizeof(info)) != 0 ||
        info.handle != handle || info.virtual_base == 0) return nullptr;
    if (bytes != nullptr) *bytes = static_cast<size_t>(info.byte_length);
    if (gpu_va != nullptr) *gpu_va = info.gpu_va;
    return reinterpret_cast<void*>(info.virtual_base);
}

extern "C" int neutrino_render_fence_status(int context, uint64_t* submitted,
                                               uint64_t* completed, int* state, int* error) {
    neutrino_render::FenceInfo info{};
    if (context < 0 || descriptor_get_property(static_cast<uint32_t>(context),
        static_cast<uint32_t>(neutrino_render::Property::FenceInfo), &info, sizeof(info)) != 0) return -1;
    if (submitted != nullptr) *submitted = info.submitted_fence;
    if (completed != nullptr) *completed = info.completed_fence;
    if (state != nullptr) *state = static_cast<int>(info.state);
    if (error != nullptr) *error = info.error;
    return 0;
}

extern "C" int neutrino_render_wait_fence(int context, uint64_t fence,
                                            uint64_t timeout_ticks) {
    const neutrino_render::FenceWait request{fence, timeout_ticks, 0, 0};
    return context < 0 ? -1 : (descriptor_set_property(static_cast<uint32_t>(context),
        static_cast<uint32_t>(neutrino_render::Property::WaitFence),
        &request, sizeof(request)) == 0 ? 0 : -1);
}
