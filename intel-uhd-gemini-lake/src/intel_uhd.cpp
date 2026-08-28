#include "intel_uhd.hpp"
#include "gen9_probe_fs.hpp"

#include <stddef.h>
#include <stdint.h>

#include "arch/x86_64/memory/paging.hpp"
#include "drivers/console/console.hpp"
#include "drivers/driver_registry.hpp"
#include "drivers/display_accel.hpp"
#include "drivers/render_accel.hpp"
#include "drivers/log/logging.hpp"
#include "drivers/pci/pci.hpp"
#include "kernel/cmdline.hpp"
#include "kernel/descriptor.hpp"
#include "kernel/memory/physical_allocator.hpp"
#include "kernel/module.hpp"
#include "lib/mem.hpp"
#include "neutrino_render.hpp"

namespace intel_uhd {

namespace {

#ifdef NEUTRINO_DYNAMIC_MODULE_INTEL_UHD
const kernel_module::Api* g_module_api = nullptr;
#endif

constexpr uint16_t kIntelVendorId = 0x8086;

#define INTEL_UHD_SUPPORTED_DEVICES(X) \
    X(0x3184, "Gemini Lake UHD 600/605") \
    X(0x3185, "Gemini Lake UHD 600/605") \
    X(0x5916, "Kaby Lake UHD 620") \
    X(0x5917, "Kaby Lake UHD 620") \
    X(0x591B, "Kaby Lake UHD 630") \
    X(0x3EA0, "Whiskey Lake UHD 620") \
    X(0x3EA9, "Coffee Lake UHD 630") \
    X(0x3E91, "Coffee Lake UHD 630") \
    X(0x3E92, "Coffee Lake UHD 630") \
    X(0x3E98, "Coffee Lake UHD 630") \
    X(0x9BC4, "Comet Lake UHD 630") \
    X(0x9BC5, "Comet Lake UHD 630")

struct DeviceInfo {
    uint16_t device_id;
    const char* name;
};

constexpr DeviceInfo kSupportedDevices[] = {
#define INTEL_UHD_DEVICE_INFO(device_id, name) {device_id, name},
    INTEL_UHD_SUPPORTED_DEVICES(INTEL_UHD_DEVICE_INFO)
#undef INTEL_UHD_DEVICE_INFO
};

constexpr driver_registry::PciMatch kPciMatches[] = {
#define INTEL_UHD_PCI_MATCH(device_id, name) \
    {.vendor = kIntelVendorId, .device = device_id, .class_code = 0x03, .subclass = driver_registry::kAnySubclass, .prog_if = driver_registry::kAnyProgIf},
    INTEL_UHD_SUPPORTED_DEVICES(INTEL_UHD_PCI_MATCH)
#undef INTEL_UHD_PCI_MATCH
};

#undef INTEL_UHD_SUPPORTED_DEVICES

constexpr uint16_t kPciCommandIo = 1u << 0;
constexpr uint16_t kPciCommandMemory = 1u << 1;
constexpr uint16_t kPciCommandBusMaster = 1u << 2;

constexpr uint64_t kMapVirtBase = 0xFFFFE10060000000ull;
constexpr size_t kMapWindowSize = 32ull * 1024 * 1024;
constexpr size_t kPageSize = 4096;
constexpr size_t kMmioMapSize = 2ull * 1024 * 1024;
constexpr size_t kBltRingSize = 16ull * 1024;
constexpr size_t kBltHwspSize = kPageSize;
// Keep the RCS backing separate from BCS.  The two engines have independent
// rings and hardware status pages; sharing either would corrupt in-flight
// commands once real render submission is enabled.
constexpr size_t kRenderRingSize = 16ull * 1024;
constexpr size_t kRenderEngineHwspSize = kPageSize;
constexpr size_t kRenderBatchSize = kPageSize;
constexpr uint32_t kRenderBatchPpgtt = 0x00100000u;
constexpr uint32_t kRenderFencePpgtt = kRenderBatchPpgtt + kPageSize;
constexpr uint32_t kRenderStatePpgtt = kRenderFencePpgtt + kPageSize;
constexpr uint32_t kRenderTargetPpgtt = kRenderStatePpgtt + kPageSize;
constexpr uint32_t kRenderTargetWidth = 64u;
constexpr uint32_t kRenderTargetHeight = 64u;
constexpr uint32_t kRenderTargetPitch = kRenderTargetWidth * sizeof(uint32_t);
constexpr size_t kRenderTargetSize =
    kRenderTargetPitch * kRenderTargetHeight;
constexpr size_t kRenderTargetPages = kRenderTargetSize / kPageSize;
// Gen9 RCS logical contexts occupy 22 pages.  Page zero is the per-process
// HWSP and page one begins the hardware register-state image.
constexpr size_t kGen9RenderContextPages = 22;
constexpr size_t kGen9RenderContextSize =
    kGen9RenderContextPages * kPageSize;
constexpr size_t kGen9PpgttRootPages = 4;
constexpr size_t kRenderBindingCount = 16;
constexpr size_t kRenderBindingBytes = 4ull * 1024 * 1024;
// Gen8+ exposes a 16 MiB GTTMMADR BAR: the first 8 MiB is MMIO and the
// second 8 MiB is the GGTT page-table aperture.  The old 2 MiB offset is the
// Gen6/7 layout and writes unrelated registers on Gemini Lake.
constexpr size_t kGgttTableOffset = 8ull * 1024 * 1024;
constexpr uint32_t kBltRingBase = 0x22000;
constexpr uint32_t kRenderRingBase = 0x02000;
constexpr uint32_t kRingTail = 0x30;
constexpr uint32_t kRingHead = 0x34;
constexpr uint32_t kRingStart = 0x38;
constexpr uint32_t kRingCtl = 0x3C;
constexpr uint32_t kRingHwsPga = 0x80;
constexpr uint32_t kRingHwstam = 0x98;
constexpr uint32_t kRingMiMode = 0x9C;
constexpr uint32_t kRingElsp = 0x230;
constexpr uint32_t kRingExeclistStatusLo = 0x234;
constexpr uint32_t kRingExeclistStatusHi = 0x238;
constexpr uint32_t kRingModeGen7 = 0x29C;
constexpr uint32_t kRingContextStatusPtr = 0x3A0;
constexpr uint32_t kRingValid = 0x1u;
constexpr uint32_t kRingStop = 1u << 8;
constexpr uint32_t kModeIdle = 1u << 9;
constexpr uint32_t kGfxRunListEnable = 1u << 15;
constexpr uint32_t kTailAddrMask = 0x001FFFF8u;
constexpr uint32_t kHeadAddrMask = 0x001FFFFCu;
constexpr uint32_t kGfxFlushCntlGen6 = 0x101008;
constexpr uint32_t kGfxFlushCntlEn = 1u << 0;
// Gemini Lake is Gen9LP.  RCS MMIO is in the Gen9 render force-wake domain;
// accesses to the ring registers are unreliable while that domain is asleep.
constexpr uint32_t kForcewakeRenderGen9 = 0xA278;
constexpr uint32_t kForcewakeAckRenderGen9 = 0x0D84;
constexpr uint32_t kForcewakeBlitterGen9 = 0xA188;
constexpr uint32_t kForcewakeAckBlitterGen9 = 0x130044;
constexpr uint32_t kForcewakeKernel = 1u << 0;
constexpr uint32_t kForcewakeKernelFallback = 1u << 15;
constexpr uint32_t kMiLoadRegisterImm = 0x11000000u;
constexpr uint32_t kMiLriForcePosted = 1u << 12;
constexpr uint32_t kMiBatchBufferEnd = 0x05000000u;
constexpr uint32_t kMiArbOnOff = 0x04000000u;
constexpr uint32_t kMiArbEnable = 1u << 0;
constexpr uint32_t kMiBatchBufferStartGen8 = 0x18800001u;
constexpr uint32_t kMiBatchPpgtt = 1u << 8;
constexpr uint32_t kPipeControl = 0x7A000004u;  // Six DWORD command.
constexpr uint32_t kPipeControlCsStall = 1u << 20;
constexpr uint32_t kPipeControlTlbInvalidate = 1u << 18;
constexpr uint32_t kPipeControlQwordWrite = 1u << 14;
constexpr uint32_t kPipeControlRenderTargetFlush = 1u << 12;
constexpr uint32_t kPipeControlInstructionInvalidate = 1u << 11;
constexpr uint32_t kPipeControlTextureInvalidate = 1u << 10;
constexpr uint32_t kPipeControlFlushEnable = 1u << 7;
constexpr uint32_t kPipeControlDcFlush = 1u << 5;
constexpr uint32_t kPipeControlVfInvalidate = 1u << 4;
constexpr uint32_t kPipeControlConstantInvalidate = 1u << 3;
constexpr uint32_t kPipeControlStateInvalidate = 1u << 2;
constexpr uint32_t kPipeControlDepthFlush = 1u << 0;
constexpr uint32_t kPipelineSelect3dGen9 = 0x69040300u;
constexpr uint32_t k3dStateAaLineParameters = 0x790A0001u;
constexpr uint32_t k3dStateDrawingRectangle = 0x79000002u;
constexpr uint32_t k3dStateWmChromakey = 0x784C0000u;
constexpr uint32_t kStateBaseAddressGen9 = 0x61010011u;
constexpr uint32_t kGen9MocsPte = 2u;
constexpr uint32_t k3dStateBindingTablePointersPs = 0x782A0000u;
constexpr uint32_t k3dStateWm = 0x78140000u;
constexpr uint32_t k3dStatePs = 0x7820000Au;
constexpr uint32_t k3dStatePsBlend = 0x784D0000u;
constexpr uint32_t k3dStatePsExtra = 0x784F0000u;
constexpr uint32_t kRenderBindingTableOffset = 0x20u;
constexpr uint32_t kRenderSurfaceStateOffset = 0x40u;
// The Gen8 engine HWSP CSB occupies DWORDs 0x10..0x1f.  Keep the probe
// breadcrumb in i915's driver-reserved HWSP area and naturally QWORD aligned.
constexpr uint32_t kRenderProbeFenceOffset = 0x100u;
constexpr uint32_t kRenderProbeFenceValue = 0x4C524331u;  // "LRC1"
constexpr uint32_t kRenderRequestDwords = 6u;
constexpr uint32_t kRenderRequestBytes =
    kRenderRequestDwords * sizeof(uint32_t);
constexpr uint32_t kRenderBatchDwords =
    73u + gen9_probe_fs::kDrawPacketDwords + 12u + 1u;
static_assert(gen9_probe_fs::kVertexPpgtt ==
              kRenderTargetPpgtt + kRenderTargetSize);
static_assert(gen9_probe_fs::kHeapOffset + gen9_probe_fs::kKernelSize <=
              gen9_probe_fs::kViewportOffset);
static_assert(gen9_probe_fs::kColorCalcOffset + 6u * sizeof(uint32_t) <=
              kPageSize);
constexpr uint32_t kGen8CtxValid = 1u << 0;
constexpr uint32_t kGen8CtxForceRestore = 1u << 2;
constexpr uint32_t kGen8CtxPrivilege = 1u << 8;
constexpr uint32_t kGen8Legacy32BitContext = 1u << 3;
constexpr uint32_t kGen9ContextId = 1u;
constexpr uint64_t kGen8PpgttPresent = 1u << 0;
constexpr uint64_t kGen8PpgttWritable = 1u << 1;
constexpr uint32_t kCtxContextControl = 0x03;
constexpr uint32_t kCtxRingHead = 0x05;
constexpr uint32_t kCtxRingTail = 0x07;
constexpr uint32_t kCtxRingStart = 0x09;
constexpr uint32_t kCtxRingCtl = 0x0B;
constexpr uint32_t kCtxPdp3Udw = 0x25;
constexpr uint32_t kCtxPdp3Ldw = 0x27;
constexpr uint32_t kCtxPdp2Udw = 0x29;
constexpr uint32_t kCtxPdp2Ldw = 0x2B;
constexpr uint32_t kCtxPdp1Udw = 0x2D;
constexpr uint32_t kCtxPdp1Ldw = 0x2F;
constexpr uint32_t kCtxPdp0Udw = 0x31;
constexpr uint32_t kCtxPdp0Ldw = 0x33;
constexpr uint32_t kGen9CtxRingMiMode = 0x54;
constexpr uint32_t kErrorInterrupt = 0x20B0;
constexpr uint32_t kErrorStatus = 0x20B8;
constexpr uint32_t kRenderIpeir = 0x2064;
constexpr uint32_t kRenderIpehr = 0x2068;
constexpr uint32_t kRenderFault = 0x4094;
constexpr uint32_t kFaultTlbData0 = 0x4B10;
constexpr uint32_t kFaultTlbData1 = 0x4B14;
constexpr char kDebugBase32[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
constexpr uint32_t kXySrcCopyBltCmd = (2u << 29) | (0x53u << 22);
constexpr uint32_t kXyColorBltCmd = (2u << 29) | (0x50u << 22);
constexpr uint32_t kBltWriteRgba = (1u << 20) | (2u << 20);
constexpr uint32_t kBltDepth32 = 3u << 24;
constexpr uint32_t kBltRopSrcCopy = 0xCCu << 16;
constexpr uint64_t kGen8GgttPagePresent = 1ull << 0;

constexpr uint32_t REG_GMCH_GMS = 0x50;
constexpr uint32_t REG_GFX_MODE = 0x2520;
constexpr uint32_t REG_GGTT_PAT = 0x40E0;
constexpr uint32_t REG_CDCLK_CTL = 0x46000;

constexpr uint32_t TRANS_HTOTAL_A = 0x60000;
constexpr uint32_t TRANS_VTOTAL_A = 0x6000C;
constexpr uint32_t TRANS_DDI_FUNC_CTL_A = 0x60400;
constexpr uint32_t PIPE_SRC_A = 0x6001C;
constexpr uint32_t PIPECONF_A = 0x70008;
constexpr uint32_t PIPE_STRIDE = 0x1000;
constexpr uint32_t PLANE_CTL_1_A = 0x70180;
constexpr uint32_t PLANE_STRIDE_1_A = 0x70188;
constexpr uint32_t PLANE_SIZE_1_A = 0x70190;
constexpr uint32_t PLANE_SURF_1_A = 0x7019C;
constexpr uint32_t PLANE_OFFSET_1_A = 0x701A4;
constexpr uint32_t PLANE_SURFLIVE_1_A = 0x701AC;

constexpr uint32_t PIPECONF_ENABLE = 1u << 31;
constexpr uint32_t DDI_FUNC_ENABLE = 1u << 31;
constexpr uint32_t PLANE_CTL_ENABLE = 1u << 31;
constexpr uint32_t PLANE_CTL_FORMAT_MASK = 0x0Fu << 24;
constexpr uint32_t PLANE_CTL_FORMAT_XRGB_8888 = 0x04u << 24;
constexpr uint32_t PLANE_CTL_ORDER_RGBX = 1u << 20;
constexpr uint32_t PLANE_CTL_TILED_MASK = 0x07u << 10;
constexpr uint32_t PLANE_CTL_TILED_LINEAR = 0x00u << 10;
constexpr uint32_t PLANE_STRIDE_MASK = 0x0FFFu;
constexpr uint32_t PLANE_STRIDE_GRANULARITY = 64u;

struct PipeState {
    bool enabled;
    uint32_t pipeconf;
    uint32_t source;
    uint32_t htotal;
    uint32_t vtotal;
    uint32_t ddi_func_ctl;
};

struct PlaneState {
    bool enabled;
    bool linear;
    bool xrgb8888;
    bool rgbx_order;
    uint32_t ctl;
    uint32_t stride;
    uint32_t size;
    uint32_t surf;
    uint32_t surf_live;
    uint32_t offset;
    uint16_t width;
    uint16_t height;
    uint16_t offset_x;
    uint16_t offset_y;
};

struct DriverState {
    bool initialized;
    bool active;
    bool blt_ready;
    pci::PciDevice device;
    const DeviceInfo* info;
    volatile uint8_t* regs;
    uint8_t* scanout_base;
    uint64_t mmio_phys;
    uint64_t aperture_phys;
    uint64_t mmio_size;
    uint64_t aperture_size;
    uint64_t scanout_phys;
    uint64_t scanout_ggtt_offset;
    size_t scanout_bytes;
    uint64_t ggtt_table_phys;
    uint64_t blt_hwsp_phys;
    uint64_t blt_ring_phys;
    uint64_t blt_hwsp_ggtt;
    uint64_t blt_ring_ggtt;
    uint64_t blt_source_ggtt;
    volatile uint64_t* blt_source_ptes;
    size_t blt_source_page_capacity;
    volatile uint32_t* blt_ring_cpu;
    uint32_t blt_ring_tail;
    uint32_t blt_reset_count;
    bool blt_disabled;
    bool render_context_ready;
    uint64_t render_engine_hwsp_phys;
    uint64_t render_context_phys;
    uint64_t render_ring_phys;
    uint64_t render_batch_phys;
    uint64_t render_state_heap_phys;
    uint64_t render_target_phys;
    uint64_t render_vertex_phys;
    uint64_t render_ppgtt_roots_phys;
    uint64_t render_ppgtt_pt_phys;
    uint64_t render_engine_hwsp_ggtt;
    uint64_t render_context_ggtt;
    uint64_t render_ring_ggtt;
    uint64_t render_batch_gpu_va;
    volatile uint32_t* render_context_cpu;
    volatile uint32_t* render_ring_cpu;
    volatile uint32_t* render_batch_cpu;
    volatile uint32_t* render_state_heap_cpu;
    volatile uint32_t* render_target_cpu;
    volatile uint32_t* render_vertex_cpu;
    uint32_t render_ring_tail;
    uint32_t render_probe_count;
    bool render_target_write_validated;
    bool render_demo_registered;
    bool render_sync_registered;
    uint16_t command;
    PipeState pipes[3];
    PlaneState planes[3];
    int active_pipe;
};

DriverState g_state{};
struct RenderBinding {
    volatile uint64_t* ptes;
    size_t page_count;
    uint64_t gpu_va;
    bool used;
};

struct RenderDebugSnapshot {
    uint32_t flags;
    uint32_t head;
    uint32_t tail;
    uint32_t ctl;
    uint32_t mode;
    uint32_t runlist;
    uint32_t els_hi;
    uint32_t els_lo;
    uint32_t csb;
    uint32_t fence_xor_expected;
    uint32_t expected;
    uint32_t eir;
    uint32_t esr;
    uint32_t ipeir;
    uint32_t ipehr;
    uint32_t fault;
    uint32_t tlb_hi;
    uint32_t tlb_lo;
};
RenderBinding g_render_bindings[kRenderBindingCount]{};
uint64_t g_map_next_virt = kMapVirtBase;
bool g_disabled = false;
bool g_render_probe_requested = false;

uint64_t align_down_u64(uint64_t value, uint64_t alignment) {
    return value & ~(alignment - 1);
}

uint16_t debug_crc16(const uint8_t* bytes, size_t length) {
    uint16_t crc = 0xFFFFu;
    for (size_t i = 0; i < length; ++i) {
        crc ^= static_cast<uint16_t>(bytes[i]) << 8;
        for (unsigned int bit = 0; bit < 8; ++bit)
            crc = (crc & 0x8000u) != 0
                      ? static_cast<uint16_t>((crc << 1) ^ 0x1021u)
                      : static_cast<uint16_t>(crc << 1);
    }
    return crc;
}

bool append_debug_varint(uint8_t* bytes, size_t capacity,
                         size_t& length, uint32_t value) {
    do {
        if (length >= capacity) return false;
        uint8_t byte = static_cast<uint8_t>(value & 0x7Fu);
        value >>= 7;
        if (value != 0) byte |= 0x80u;
        bytes[length++] = byte;
    } while (value != 0);
    return true;
}

bool encode_render_debug(const RenderDebugSnapshot& snapshot,
                         char* output, size_t capacity) {
    uint8_t packed[96]{};
    size_t packed_length = 0;
    packed[packed_length++] = 1;  // RCSDBG schema version.
    const uint32_t values[] = {
        snapshot.flags,
        snapshot.head >> 3,
        snapshot.tail >> 3,
        snapshot.ctl,
        snapshot.mode,
        snapshot.runlist,
        snapshot.els_hi,
        snapshot.els_lo,
        snapshot.csb,
        snapshot.fence_xor_expected,
        snapshot.expected,
        snapshot.eir,
        snapshot.esr,
        snapshot.ipeir,
        snapshot.ipehr,
        snapshot.fault,
        snapshot.tlb_hi,
        snapshot.tlb_lo,
    };
    for (uint32_t value : values)
        if (!append_debug_varint(packed, sizeof(packed),
                                 packed_length, value))
            return false;
    const uint16_t crc = debug_crc16(packed, packed_length);
    if (packed_length + 2 > sizeof(packed)) return false;
    packed[packed_length++] = static_cast<uint8_t>(crc >> 8);
    packed[packed_length++] = static_cast<uint8_t>(crc);

    size_t out = 0;
    uint32_t accumulator = 0;
    unsigned int bits = 0;
    unsigned int group = 0;
    for (size_t i = 0; i < packed_length; ++i) {
        accumulator = (accumulator << 8) | packed[i];
        bits += 8;
        while (bits >= 5) {
            bits -= 5;
            if (group == 5) {
                if (out + 1 >= capacity) return false;
                output[out++] = '-';
                group = 0;
            }
            if (out + 1 >= capacity) return false;
            output[out++] = kDebugBase32[(accumulator >> bits) & 0x1Fu];
            ++group;
        }
    }
    if (bits != 0) {
        if (group == 5) {
            if (out + 1 >= capacity) return false;
            output[out++] = '-';
        }
        if (out + 1 >= capacity) return false;
        output[out++] = kDebugBase32[(accumulator << (5 - bits)) & 0x1Fu];
    }
    if (out >= capacity) return false;
    output[out] = '\0';
    return true;
}

uint64_t align_up_u64(uint64_t value, uint64_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

const DeviceInfo* lookup_device_info(uint16_t device_id) {
    for (size_t i = 0; i < sizeof(kSupportedDevices) / sizeof(kSupportedDevices[0]); ++i) {
        if (kSupportedDevices[i].device_id == device_id) {
            return &kSupportedDevices[i];
        }
    }
    return nullptr;
}

uint64_t pci_bar_base(const pci::PciDevice& device, uint8_t bar_index) {
    if (bar_index >= 6) {
        return 0;
    }

    uint8_t reg = static_cast<uint8_t>(0x10 + (bar_index * 4));
    uint32_t low = pci::read_config32(device, reg);
    if ((low & 0x1u) != 0) {
        return 0;
    }

    uint64_t base = static_cast<uint64_t>(low & ~0xFu);
    uint32_t bar_type = (low >> 1) & 0x3u;
    if (bar_type == 0x2u) {
        if (bar_index + 1 >= 6) {
            return 0;
        }
        uint32_t high = pci::read_config32(device, static_cast<uint8_t>(reg + 4));
        base |= static_cast<uint64_t>(high) << 32;
    }
    return base;
}

uint64_t pci_bar_size(const pci::PciDevice& device, uint8_t bar_index) {
    if (bar_index >= 6) {
        return 0;
    }

    const uint8_t reg = static_cast<uint8_t>(0x10 + (bar_index * 4));
    const uint32_t original_low = pci::read_config32(device, reg);
    if ((original_low & 0x1u) != 0) {
        return 0;
    }

    const bool is_64 = ((original_low >> 1) & 0x3u) == 0x2u;
    uint32_t original_high = 0;
    if (is_64) {
        if (bar_index + 1 >= 6) {
            return 0;
        }
        original_high = pci::read_config32(device, static_cast<uint8_t>(reg + 4));
    }

    pci::write_config32(device, reg, 0xFFFFFFFFu);
    uint32_t size_low = pci::read_config32(device, reg);
    uint32_t size_high = 0;
    if (is_64) {
        pci::write_config32(device, static_cast<uint8_t>(reg + 4), 0xFFFFFFFFu);
        size_high = pci::read_config32(device, static_cast<uint8_t>(reg + 4));
    }

    pci::write_config32(device, reg, original_low);
    if (is_64) {
        pci::write_config32(device, static_cast<uint8_t>(reg + 4), original_high);
    }

    uint64_t mask = static_cast<uint64_t>(size_low & ~0xFu);
    if (is_64) {
        mask |= static_cast<uint64_t>(size_high) << 32;
    }
    if (mask == 0) {
        return 0;
    }
    return (~mask) + 1ull;
}

uint8_t* map_physical_range(uint64_t phys_base,
                            size_t length,
                            uint64_t page_flags,
                            const char* label) {
    if (phys_base == 0 || length == 0) {
        return nullptr;
    }

    uint64_t page_phys = align_down_u64(phys_base, kPageSize);
    uint64_t page_end = align_up_u64(phys_base + length, kPageSize);
    size_t page_count = static_cast<size_t>((page_end - page_phys) / kPageSize);
    if (page_count == 0) {
        page_count = 1;
    }

    uint64_t virt_base = g_map_next_virt;
    uint64_t virt_end =
        virt_base + static_cast<uint64_t>(page_count) * kPageSize;
    if (virt_end - kMapVirtBase > kMapWindowSize) {
        log_message(LogLevel::Warn,
                    "intel-uhd: virtual window exhausted while mapping %s",
                    (label != nullptr) ? label : "range");
        return nullptr;
    }

    for (size_t i = 0; i < page_count; ++i) {
        uint64_t phys = page_phys + static_cast<uint64_t>(i) * kPageSize;
        uint64_t virt = virt_base + static_cast<uint64_t>(i) * kPageSize;
        if (!paging_map_page(virt,
                             phys,
                             page_flags | PAGE_FLAG_NO_EXECUTE)) {
            log_message(LogLevel::Warn,
                        "intel-uhd: failed to map %s page phys=%016llx",
                        (label != nullptr) ? label : "range",
                        static_cast<unsigned long long>(phys));
            return nullptr;
        }
    }

    g_map_next_virt = virt_end;
    return reinterpret_cast<uint8_t*>(virt_base + (phys_base - page_phys));
}

volatile uint8_t* map_bar_window(const pci::PciDevice& device,
                                 uint8_t bar_index,
                                 size_t length) {
    uint64_t bar_base = pci_bar_base(device, bar_index);
    if (bar_base == 0 || length == 0) {
        return nullptr;
    }

    const uint64_t mmio_flags =
        PAGE_FLAG_WRITE | PAGE_FLAG_WRITE_THROUGH | PAGE_FLAG_CACHE_DISABLE;
    return reinterpret_cast<volatile uint8_t*>(
        map_physical_range(bar_base, length, mmio_flags, "MMIO"));
}

uint32_t mmio_read32(uint32_t reg) {
    if (g_state.regs == nullptr) {
        return 0xFFFFFFFFu;
    }
    return *reinterpret_cast<volatile uint32_t*>(
        const_cast<volatile uint8_t*>(g_state.regs) + reg);
}

void mmio_write32(uint32_t reg, uint32_t value) {
    if (g_state.regs == nullptr) {
        return;
    }
    *reinterpret_cast<volatile uint32_t*>(
        const_cast<volatile uint8_t*>(g_state.regs) + reg) = value;
}

bool wait_for_blt_idle(uint32_t timeout_iterations = 1000000u) {
    if (g_state.regs == nullptr) {
        return false;
    }
    for (uint32_t i = 0; i < timeout_iterations; ++i) {
        uint32_t head = mmio_read32(kBltRingBase + kRingHead) & kHeadAddrMask;
        uint32_t tail = mmio_read32(kBltRingBase + kRingTail) & kTailAddrMask;
        uint32_t mode = mmio_read32(kBltRingBase + kRingMiMode);
        if (head == tail && (mode & kModeIdle) != 0) {
            return true;
        }
        asm volatile("pause");
    }
    return false;
}

bool wait_for_ring_idle(uint32_t ring_base,
                        uint32_t timeout_iterations = 1000000u) {
    if (g_state.regs == nullptr) return false;
    for (uint32_t i = 0; i < timeout_iterations; ++i) {
        const uint32_t head = mmio_read32(ring_base + kRingHead) & kHeadAddrMask;
        const uint32_t tail = mmio_read32(ring_base + kRingTail) & kTailAddrMask;
        const uint32_t mode = mmio_read32(ring_base + kRingMiMode);
        if (head == tail && (mode & kModeIdle) != 0) return true;
        asm volatile("pause");
    }
    return false;
}

bool wait_for_mmio_mask(uint32_t reg, uint32_t mask, uint32_t expected,
                        uint32_t timeout_iterations = 1000000u) {
    for (uint32_t i = 0; i < timeout_iterations; ++i) {
        if ((mmio_read32(reg) & mask) == expected) return true;
        asm volatile("pause");
    }
    return false;
}

bool wait_for_cpu_value(volatile uint32_t* value, uint32_t expected,
                        uint32_t timeout_iterations = 1000000u) {
    if (value == nullptr) return false;
    for (uint32_t i = 0; i < timeout_iterations; ++i) {
        if (*value == expected) return true;
        asm volatile("pause");
    }
    return false;
}

uint32_t forcewake_enable_value(uint32_t bits) {
    return (bits << 16) | bits;
}

uint32_t forcewake_disable_value(uint32_t bits) {
    return bits << 16;
}

bool acquire_forcewake(uint32_t request, uint32_t ack, const char* engine) {
    if (g_state.regs == nullptr) return false;
    // GLK shares the known Gen9 force-wake ACK race.  Toggle the fallback
    // bit first, then retain the kernel bit through RCS setup and retirement.
    mmio_write32(request,
                 forcewake_enable_value(kForcewakeKernelFallback));
    if (!wait_for_mmio_mask(ack,
                            kForcewakeKernelFallback,
                            kForcewakeKernelFallback)) {
        log_message(LogLevel::Warn,
                    "intel-uhd: %s force-wake fallback ACK timed out ack=%08x",
                    engine, static_cast<unsigned int>(mmio_read32(ack)));
        mmio_write32(request,
                     forcewake_disable_value(kForcewakeKernelFallback));
        return false;
    }
    mmio_write32(request,
                 forcewake_disable_value(kForcewakeKernelFallback));
    mmio_write32(request,
                 forcewake_enable_value(kForcewakeKernel));
    if (wait_for_mmio_mask(ack, kForcewakeKernel,
                           kForcewakeKernel))
        return true;
    log_message(LogLevel::Warn,
                "intel-uhd: %s force-wake ACK timed out ack=%08x",
                engine, static_cast<unsigned int>(mmio_read32(ack)));
    mmio_write32(request,
                 forcewake_disable_value(kForcewakeKernel));
    return false;
}

void release_forcewake(uint32_t request) {
    if (g_state.regs != nullptr)
        mmio_write32(request,
                     forcewake_disable_value(kForcewakeKernel));
}

bool acquire_render_forcewake() {
    return acquire_forcewake(kForcewakeRenderGen9,
                             kForcewakeAckRenderGen9, "RCS");
}

void release_render_forcewake() {
    release_forcewake(kForcewakeRenderGen9);
}

bool acquire_blt_forcewake() {
    return acquire_forcewake(kForcewakeBlitterGen9,
                             kForcewakeAckBlitterGen9, "BCS");
}

void release_blt_forcewake() {
    release_forcewake(kForcewakeBlitterGen9);
}

bool program_ggtt_pages(uint64_t ggtt_offset,
                        uint64_t phys_base,
                        size_t length) {
    if (g_state.ggtt_table_phys == 0 || length == 0) {
        return false;
    }
    const uint64_t first_entry = ggtt_offset / kPageSize;
    const uint64_t page_count = align_up_u64(length, kPageSize) / kPageSize;
    const uint64_t pte_phys = g_state.ggtt_table_phys + first_entry * sizeof(uint64_t);
    auto* ptes = reinterpret_cast<volatile uint64_t*>(
        map_physical_range(pte_phys,
                           static_cast<size_t>(page_count * sizeof(uint64_t)),
                           PAGE_FLAG_WRITE | PAGE_FLAG_WRITE_THROUGH |
                               PAGE_FLAG_CACHE_DISABLE,
                           "ggtt-ptes"));
    if (ptes == nullptr) {
        return false;
    }
    for (uint64_t i = 0; i < page_count; ++i) {
        const uint64_t expected =
            (phys_base + i * kPageSize) | kGen8GgttPagePresent;
        ptes[i] = expected;
        if (ptes[i] != expected) {
            log_message(LogLevel::Warn,
                        "intel-uhd: GGTT PTE readback failed entry=%llu wrote=%016llx read=%016llx",
                        static_cast<unsigned long long>(first_entry + i),
                        static_cast<unsigned long long>(expected),
                        static_cast<unsigned long long>(ptes[i]));
            return false;
        }
    }
    mmio_write32(kGfxFlushCntlGen6, kGfxFlushCntlEn);
    (void)mmio_read32(kGfxFlushCntlGen6);
    return true;
}

bool setup_blt_engine() {
    if (g_state.blt_ready) {
        return true;
    }
    if (g_state.blt_disabled) return false;
    // A firmware primary plane at GGTT offset zero is valid (and common).
    // scanout_bytes, not its offset, tells us whether scanout was adopted.
    if (g_state.scanout_bytes == 0 || g_state.aperture_size == 0 ||
        g_state.ggtt_table_phys == 0) {
        return false;
    }

    const uint64_t reserve_start =
        align_up_u64(g_state.scanout_ggtt_offset + g_state.scanout_bytes + kPageSize,
                     kPageSize);
    const uint64_t reserve_bytes = kBltHwspSize + kBltRingSize;
    if (reserve_start + reserve_bytes > g_state.aperture_size) {
        log_message(LogLevel::Warn,
                    "intel-uhd: no GGTT space for BLT ring offset=%016llx size=%llu aperture=%llu",
                    static_cast<unsigned long long>(reserve_start),
                    static_cast<unsigned long long>(reserve_bytes),
                    static_cast<unsigned long long>(g_state.aperture_size));
        return false;
    }

    const size_t hwsp_pages = kBltHwspSize / kPageSize;
    const size_t ring_pages = kBltRingSize / kPageSize;
    if (g_state.blt_hwsp_phys == 0 || g_state.blt_ring_phys == 0) {
        g_state.blt_hwsp_phys = memory::alloc_kernel_block_pages(hwsp_pages);
        g_state.blt_ring_phys = memory::alloc_kernel_block_pages(ring_pages);
        if (g_state.blt_hwsp_phys == 0 || g_state.blt_ring_phys == 0) {
            log_message(LogLevel::Warn, "intel-uhd: failed to allocate BLT backing pages");
            return false;
        }
    }

    auto* hwsp = static_cast<uint8_t*>(paging_phys_to_virt(g_state.blt_hwsp_phys));
    auto* ring = static_cast<uint32_t*>(paging_phys_to_virt(g_state.blt_ring_phys));
    memset(hwsp, 0, kBltHwspSize);
    memset(ring, 0, kBltRingSize);

    g_state.blt_hwsp_ggtt = reserve_start;
    g_state.blt_ring_ggtt = reserve_start + kBltHwspSize;
    if (!program_ggtt_pages(g_state.blt_hwsp_ggtt, g_state.blt_hwsp_phys, kBltHwspSize) ||
        !program_ggtt_pages(g_state.blt_ring_ggtt, g_state.blt_ring_phys, kBltRingSize)) {
        return false;
    }

    if (!acquire_blt_forcewake()) {
        g_state.blt_disabled = true;
        return false;
    }
    mmio_write32(kBltRingBase + kRingHead, mmio_read32(kBltRingBase + kRingTail));
    (void)mmio_read32(kBltRingBase + kRingHead);
    mmio_write32(kBltRingBase + kRingCtl, 0);
    (void)mmio_read32(kBltRingBase + kRingCtl);
    mmio_write32(kBltRingBase + kRingHead, 0);
    mmio_write32(kBltRingBase + kRingTail, 0);
    mmio_write32(kBltRingBase + kRingHwsPga, static_cast<uint32_t>(g_state.blt_hwsp_ggtt));
    mmio_write32(kBltRingBase + kRingHwstam, 0xFFFFFFFFu);
    mmio_write32(kBltRingBase + kRingStart, static_cast<uint32_t>(g_state.blt_ring_ggtt));
    mmio_write32(kBltRingBase + kRingCtl,
                 static_cast<uint32_t>((kBltRingSize - kPageSize) | kRingValid));
    (void)mmio_read32(kBltRingBase + kRingCtl);
    mmio_write32(kBltRingBase + kRingMiMode, mmio_read32(kBltRingBase + kRingMiMode) & ~kRingStop);
    (void)mmio_read32(kBltRingBase + kRingMiMode);
    release_blt_forcewake();

    g_state.blt_ring_cpu = ring;
    g_state.blt_ring_tail = 0;
    g_state.blt_ready = true;
    log_message(LogLevel::Info,
                "intel-uhd: BLT ring ready ring=%016llx hwsp=%016llx",
                static_cast<unsigned long long>(g_state.blt_ring_ggtt),
                static_cast<unsigned long long>(g_state.blt_hwsp_ggtt));
    return true;
}

uint64_t render_binding_ggtt_base() {
    // Reserve the complete RCS area even when the optional probe is disabled.
    // This makes later activation unable to overlap an already-bound BO.
    return align_up_u64(g_state.blt_ring_ggtt + kBltRingSize +
                            kRenderEngineHwspSize + kGen9RenderContextSize +
                            kRenderRingSize,
                        kPageSize);
}

bool is_gemini_lake() {
    return g_state.device.device == 0x3184 || g_state.device.device == 0x3185;
}

uint32_t masked_enable(uint32_t bits) {
    return (bits << 16) | bits;
}

uint32_t masked_disable(uint32_t bits) {
    return bits << 16;
}

void flush_cpu_cache(const void* address, size_t length) {
    constexpr size_t kCacheLineSize = 64;
    const uintptr_t start = reinterpret_cast<uintptr_t>(address);
    const uintptr_t first = start & ~(kCacheLineSize - 1);
    const uintptr_t end = start + length;
    for (uintptr_t current = first; current < end;
         current += kCacheLineSize) {
        asm volatile("clflush (%0)" : : "r"(current) : "memory");
    }
    asm volatile("mfence" : : : "memory");
}

void emit_context_lri(volatile uint32_t* state,
                      size_t& cursor,
                      const uint16_t* offsets,
                      size_t count,
                      bool force_posted) {
    state[cursor++] = kMiLoadRegisterImm |
                      static_cast<uint32_t>(count * 2 - 1) |
                      (force_posted ? kMiLriForcePosted : 0u);
    for (size_t i = 0; i < count; ++i) {
        state[cursor++] = kRenderRingBase + offsets[i];
        state[cursor++] = 0;
    }
}

bool initialize_gen9_render_context(volatile uint32_t* context,
                                    uint64_t ring_ggtt,
                                    uint64_t ppgtt_roots_phys) {
    if (context == nullptr || ring_ggtt > UINT32_MAX ||
        (ring_ggtt & (kPageSize - 1)) != 0 ||
        (ppgtt_roots_phys & (kPageSize - 1)) != 0) {
        return false;
    }

    // Linux i915's gen9_rcs_offsets layout.  The GPU consumes page one as a
    // sequence of MI_LOAD_REGISTER_IMM commands during the first restore and
    // subsequently overwrites it with the saved context image.
    static constexpr uint16_t kGroup0[] = {
        0x244, 0x034, 0x030, 0x038, 0x03C, 0x168, 0x140,
        0x110, 0x11C, 0x114, 0x118, 0x1C0, 0x1C4, 0x1C8,
    };
    static constexpr uint16_t kGroup1[] = {
        0x3A8, 0x28C, 0x288, 0x284, 0x280,
        0x27C, 0x278, 0x274, 0x270,
    };
    static constexpr uint16_t kGroup2[] = {0x0C8};
    static constexpr uint16_t kGroup3[] = {
        0x028, 0x09C, 0x0C0, 0x178, 0x17C, 0x358, 0x170,
        0x150, 0x154, 0x158, 0x41C, 0x600, 0x604, 0x608,
        0x60C, 0x610, 0x614, 0x618, 0x61C, 0x620, 0x624,
        0x628, 0x62C, 0x630, 0x634, 0x638, 0x63C, 0x640,
        0x644, 0x648, 0x64C, 0x650, 0x654, 0x658, 0x65C,
        0x660, 0x664, 0x668, 0x66C, 0x670, 0x674, 0x678,
        0x67C, 0x068,
    };

    volatile uint32_t* state = context + kPageSize / sizeof(uint32_t);
    size_t cursor = 1;
    emit_context_lri(state, cursor, kGroup0,
                     sizeof(kGroup0) / sizeof(kGroup0[0]), true);
    cursor += 3;
    emit_context_lri(state, cursor, kGroup1,
                     sizeof(kGroup1) / sizeof(kGroup1[0]), true);
    cursor += 13;
    emit_context_lri(state, cursor, kGroup2,
                     sizeof(kGroup2) / sizeof(kGroup2[0]), false);
    cursor += 13;
    emit_context_lri(state, cursor, kGroup3,
                     sizeof(kGroup3) / sizeof(kGroup3[0]), true);
    if (cursor >= kPageSize / sizeof(uint32_t)) return false;
    // A newly initialized LRC uses the register-state page as a hardware
    // batch.  Without this terminator the restore parser walks beyond the
    // generated LRIs and reports RING_ESR.I915_ERROR_INSTRUCTION.
    state[cursor++] = kMiBatchBufferEnd;

    // This context has no captured golden render state, so its first load must
    // inhibit restoration of the zero-filled extended engine context.  The
    // logical register state below is still restored.  This is i915's
    // inhibit=true CTX_CONTEXT_CONTROL value: mask bits 0..3, enable restore
    // inhibit and synchronous-switch inhibit, and disable save inhibit/RS.
    state[kCtxContextControl] = 0x000F0009u;
    state[kCtxRingHead] = 0;
    state[kCtxRingTail] = 0;
    state[kCtxRingStart] = static_cast<uint32_t>(ring_ggtt);
    state[kCtxRingCtl] =
        static_cast<uint32_t>((kRenderRingSize - kPageSize) | kRingValid);

    const uint64_t pdp0 = ppgtt_roots_phys;
    const uint64_t pdp1 = pdp0 + kPageSize;
    const uint64_t pdp2 = pdp1 + kPageSize;
    const uint64_t pdp3 = pdp2 + kPageSize;
    state[kCtxPdp3Udw] = static_cast<uint32_t>(pdp3 >> 32);
    state[kCtxPdp3Ldw] = static_cast<uint32_t>(pdp3);
    state[kCtxPdp2Udw] = static_cast<uint32_t>(pdp2 >> 32);
    state[kCtxPdp2Ldw] = static_cast<uint32_t>(pdp2);
    state[kCtxPdp1Udw] = static_cast<uint32_t>(pdp1 >> 32);
    state[kCtxPdp1Ldw] = static_cast<uint32_t>(pdp1);
    state[kCtxPdp0Udw] = static_cast<uint32_t>(pdp0 >> 32);
    state[kCtxPdp0Ldw] = static_cast<uint32_t>(pdp0);
    state[kGen9CtxRingMiMode + 1] = masked_disable(kRingStop);
    return true;
}

bool validate_gen9_context_aperture(uint64_t context_ggtt,
                                    const volatile uint32_t* context) {
    constexpr size_t kContextRestoreDwords = 171;
    if (context == nullptr || g_state.aperture_phys == 0 ||
        context_ggtt > g_state.aperture_size ||
        2 * kPageSize > g_state.aperture_size - context_ggtt ||
        context_ggtt > UINT64_MAX - g_state.aperture_phys) {
        return false;
    }
    auto* aperture = reinterpret_cast<volatile uint32_t*>(map_physical_range(
        g_state.aperture_phys + context_ggtt,
        2 * kPageSize,
        PAGE_FLAG_WRITE | PAGE_FLAG_WRITE_THROUGH | PAGE_FLAG_CACHE_DISABLE,
        "Gen9 LRC aperture"));
    if (aperture == nullptr) return false;

    const size_t state_base = kPageSize / sizeof(uint32_t);
    for (size_t i = 0; i < kContextRestoreDwords; ++i) {
        const uint32_t cpu_value = context[state_base + i];
        const uint32_t aperture_value = aperture[state_base + i];
        if (cpu_value != aperture_value) {
            log_message(LogLevel::Warn,
                        "intel-uhd: Gen9 LRC aperture mismatch dw=%zu cpu=%08x gpu=%08x",
                        i,
                        static_cast<unsigned int>(cpu_value),
                        static_cast<unsigned int>(aperture_value));
            return false;
        }
    }
    return true;
}

void emit_gen9_render_breadcrumb(volatile uint32_t* ring,
                                 uint64_t fence_gpu_va,
                                 uint32_t fence_value) {
    ring[0] = kPipeControl;
    ring[1] = kPipeControlCsStall |
              kPipeControlTlbInvalidate |
              kPipeControlRenderTargetFlush |
              kPipeControlDepthFlush |
              kPipeControlDcFlush;
    ring[2] = 0;
    ring[3] = 0;
    ring[4] = 0;
    ring[5] = 0;
    ring[6] = kPipeControl;
    ring[7] = kPipeControlFlushEnable |
              kPipeControlCsStall |
              kPipeControlQwordWrite;
    ring[8] = static_cast<uint32_t>(fence_gpu_va);
    ring[9] = static_cast<uint32_t>(fence_gpu_va >> 32);
    ring[10] = fence_value;
    ring[11] = 0;
}

void emit_gen9_3d_probe_batch(volatile uint32_t* batch,
                              uint64_t fence_gpu_va,
                              uint32_t fence_value) {
    // PIPELINE_SELECT transitions require a stalling write-cache flush,
    // followed by a separate read-only cache invalidation.  The QWORD write
    // supplies the required post-sync operation for the invalidation packet.
    batch[0] = kPipeControl;
    batch[1] = kPipeControlCsStall |
               kPipeControlRenderTargetFlush |
               kPipeControlDepthFlush |
               kPipeControlDcFlush;
    batch[2] = 0;
    batch[3] = 0;
    batch[4] = 0;
    batch[5] = 0;
    // Gen9 requires a completely empty PIPE_CONTROL immediately before any
    // PIPE_CONTROL that enables VF cache invalidation.
    batch[6] = kPipeControl;
    batch[7] = 0;
    batch[8] = 0;
    batch[9] = 0;
    batch[10] = 0;
    batch[11] = 0;
    batch[12] = kPipeControl;
    batch[13] = kPipeControlCsStall |
                kPipeControlTlbInvalidate |
                kPipeControlInstructionInvalidate |
                kPipeControlTextureInvalidate |
                kPipeControlVfInvalidate |
                kPipeControlConstantInvalidate |
                kPipeControlStateInvalidate |
                kPipeControlQwordWrite;
    batch[14] = static_cast<uint32_t>(fence_gpu_va);
    batch[15] = static_cast<uint32_t>(fence_gpu_va >> 32);
    batch[16] = 0;
    batch[17] = 0;
    batch[18] = kPipelineSelect3dGen9;
    // Mesa requires a CS-stalling render/DC flush before changing surface
    // state bases.  Install one coherent private page for every Gen9 heap;
    // later probes will subdivide it into surface, binding, and shader state.
    batch[19] = kPipeControl;
    batch[20] = kPipeControlCsStall |
                kPipeControlRenderTargetFlush |
                kPipeControlDcFlush;
    batch[21] = 0;
    batch[22] = 0;
    batch[23] = 0;
    batch[24] = 0;
    const uint32_t base = kRenderStatePpgtt |
                          (kGen9MocsPte << 4) | 1u;
    const uint32_t size = (1u << 12) | 1u;
    batch[25] = kStateBaseAddressGen9;
    batch[26] = base;
    batch[27] = 0;
    batch[28] = kGen9MocsPte << 16;
    batch[29] = base;
    batch[30] = 0;
    batch[31] = base;
    batch[32] = 0;
    batch[33] = base;
    batch[34] = 0;
    batch[35] = base;
    batch[36] = 0;
    batch[37] = size;
    batch[38] = size;
    batch[39] = size;
    batch[40] = size;
    batch[41] = base;
    batch[42] = 0;
    batch[43] = 1u << 12;
    // Install the render-target binding and the fixed-function defaults Mesa
    // uses around a fragment program.
    batch[44] = k3dStateBindingTablePointersPs;
    batch[45] = kRenderBindingTableOffset;
    batch[46] = k3dStateAaLineParameters;
    batch[47] = 0;
    batch[48] = 0;
    batch[49] = k3dStateDrawingRectangle;
    batch[50] = 0;
    batch[51] = ((kRenderTargetHeight - 1) << 16) |
                (kRenderTargetWidth - 1);
    batch[52] = 0;
    batch[53] = k3dStateWmChromakey;
    batch[54] = 0;
    batch[55] = k3dStateWm;
    batch[56] = 0;
    // Mesa Gen9 3DSTATE_PS for the matching kernel in gen9_probe_fs.hpp:
    // KSP0 is SIMD8 at heap+0x100 and KSP2 is SIMD16 at heap+0x180.  Both
    // variants start their payload at GRF 2 and require neither constants nor
    // scratch.  Gemini Lake has 64 threads per pixel-shader dispatcher.
    batch[57] = k3dStatePs;
    batch[58] = gen9_probe_fs::kHeapOffset;
    batch[59] = 0;
    batch[60] = 1u << 18;  // One binding-table entry.
    batch[61] = 0;
    batch[62] = 0;
    batch[63] = (63u << 23) | (1u << 1) | (1u << 0);
    batch[64] = (gen9_probe_fs::kDispatchGrfStart << 16) |
                gen9_probe_fs::kDispatchGrfStart;
    batch[65] = 0;
    batch[66] = 0;
    batch[67] = gen9_probe_fs::kHeapOffset +
                gen9_probe_fs::kSimd16Offset;
    batch[68] = 0;
    batch[69] = k3dStatePsExtra;
    batch[70] = (1u << 31) | (1u << 8);  // Valid; position attribute enabled.
    batch[71] = k3dStatePsBlend;
    batch[72] = 1u << 30;  // Render target zero is writable.
    for (size_t i = 0; i < gen9_probe_fs::kDrawPacketDwords; ++i)
        batch[73 + i] = gen9_probe_fs::kDrawPackets[i];
    constexpr size_t breadcrumb = 73 + gen9_probe_fs::kDrawPacketDwords;
    emit_gen9_render_breadcrumb(batch + breadcrumb,
                                fence_gpu_va, fence_value);
    batch[breadcrumb + 12] = kMiBatchBufferEnd;
}

bool setup_render_engine() {
    if (g_state.render_context_ready) return true;
    if (!g_state.active || !is_gemini_lake() || !setup_blt_engine()) return false;

    const uint64_t engine_hwsp_ggtt =
        align_up_u64(g_state.blt_ring_ggtt + kBltRingSize, kPageSize);
    const uint64_t context_ggtt = engine_hwsp_ggtt + kRenderEngineHwspSize;
    const uint64_t ring_ggtt = context_ggtt + kGen9RenderContextSize;
    if (ring_ggtt > g_state.aperture_size ||
        kRenderRingSize > g_state.aperture_size - ring_ggtt ||
        context_ggtt > UINT32_MAX || ring_ggtt > UINT32_MAX) {
        log_message(LogLevel::Warn,
                    "intel-uhd: no 32-bit GGTT space for Gen9 RCS context");
        return false;
    }

    if (g_state.render_engine_hwsp_phys == 0 ||
        g_state.render_context_phys == 0 ||
        g_state.render_ring_phys == 0 ||
        g_state.render_batch_phys == 0 ||
        g_state.render_state_heap_phys == 0 ||
        g_state.render_target_phys == 0 ||
        g_state.render_vertex_phys == 0 ||
        g_state.render_ppgtt_roots_phys == 0 ||
        g_state.render_ppgtt_pt_phys == 0) {
        g_state.render_engine_hwsp_phys = memory::alloc_kernel_block_pages(1);
        g_state.render_context_phys =
            memory::alloc_kernel_block_pages(kGen9RenderContextPages);
        g_state.render_ring_phys =
            memory::alloc_kernel_block_pages(kRenderRingSize / kPageSize);
        g_state.render_batch_phys = memory::alloc_kernel_block_pages(1);
        g_state.render_state_heap_phys = memory::alloc_kernel_block_pages(1);
        g_state.render_target_phys =
            memory::alloc_kernel_block_pages(kRenderTargetPages);
        g_state.render_vertex_phys = memory::alloc_kernel_block_pages(1);
        g_state.render_ppgtt_roots_phys =
            memory::alloc_kernel_block_pages(kGen9PpgttRootPages);
        g_state.render_ppgtt_pt_phys = memory::alloc_kernel_block_pages(1);
        if (g_state.render_engine_hwsp_phys == 0 ||
            g_state.render_context_phys == 0 ||
            g_state.render_ring_phys == 0 ||
            g_state.render_batch_phys == 0 ||
            g_state.render_state_heap_phys == 0 ||
            g_state.render_target_phys == 0 ||
            g_state.render_vertex_phys == 0 ||
            g_state.render_ppgtt_roots_phys == 0 ||
            g_state.render_ppgtt_pt_phys == 0) {
            log_message(LogLevel::Warn,
                        "intel-uhd: failed to allocate Gen9 RCS context backing");
            return false;
        }
    }

    auto* engine_hwsp = static_cast<uint8_t*>(
        paging_phys_to_virt(g_state.render_engine_hwsp_phys));
    auto* context = static_cast<uint32_t*>(
        paging_phys_to_virt(g_state.render_context_phys));
    auto* ring = static_cast<uint32_t*>(
        paging_phys_to_virt(g_state.render_ring_phys));
    auto* batch = static_cast<uint32_t*>(
        paging_phys_to_virt(g_state.render_batch_phys));
    auto* state_heap = static_cast<uint32_t*>(
        paging_phys_to_virt(g_state.render_state_heap_phys));
    auto* render_target = static_cast<uint32_t*>(
        paging_phys_to_virt(g_state.render_target_phys));
    auto* vertex_data = static_cast<uint32_t*>(
        paging_phys_to_virt(g_state.render_vertex_phys));
    auto* ppgtt_roots = static_cast<uint8_t*>(
        paging_phys_to_virt(g_state.render_ppgtt_roots_phys));
    auto* ppgtt_pt = static_cast<uint8_t*>(
        paging_phys_to_virt(g_state.render_ppgtt_pt_phys));
    memset(engine_hwsp, 0, kRenderEngineHwspSize);
    memset(context, 0, kGen9RenderContextSize);
    memset(ring, 0, kRenderRingSize);
    memset(batch, 0, kRenderBatchSize);
    memset(state_heap, 0, kPageSize);
    memset(render_target, 0, kRenderTargetSize);
    memset(vertex_data, 0, kPageSize);
    memset(ppgtt_roots, 0, kGen9PpgttRootPages * kPageSize);
    memset(ppgtt_pt, 0, kPageSize);
    auto* pdp0 = reinterpret_cast<uint64_t*>(ppgtt_roots);
    auto* page_table = reinterpret_cast<uint64_t*>(ppgtt_pt);
    const size_t batch_pte =
        (kRenderBatchPpgtt >> 12) & ((kPageSize / sizeof(uint64_t)) - 1);
    pdp0[0] = g_state.render_ppgtt_pt_phys |
              kGen8PpgttPresent | kGen8PpgttWritable;
    page_table[batch_pte] = g_state.render_batch_phys |
                            kGen8PpgttPresent | kGen8PpgttWritable;
    page_table[batch_pte + 1] = g_state.render_engine_hwsp_phys |
                                kGen8PpgttPresent | kGen8PpgttWritable;
    page_table[batch_pte + 2] = g_state.render_state_heap_phys |
                                kGen8PpgttPresent | kGen8PpgttWritable;
    for (size_t page = 0; page < kRenderTargetPages; ++page) {
        page_table[batch_pte + 3 + page] =
            (g_state.render_target_phys + page * kPageSize) |
            kGen8PpgttPresent | kGen8PpgttWritable;
    }
    page_table[batch_pte + 3 + kRenderTargetPages] =
        g_state.render_vertex_phys |
        kGen8PpgttPresent | kGen8PpgttWritable;
    auto* binding_table = reinterpret_cast<uint32_t*>(
        reinterpret_cast<uint8_t*>(state_heap) + kRenderBindingTableOffset);
    binding_table[0] = kRenderSurfaceStateOffset;
    auto* surface_state = reinterpret_cast<uint32_t*>(
        reinterpret_cast<uint8_t*>(state_heap) + kRenderSurfaceStateOffset);
    // Gen9 RENDER_SURFACE_STATE: linear 64x64 B8G8R8A8_UNORM render target,
    // one array layer, PTE-coherent MOCS, and identity shader channels.
    surface_state[0] = (1u << 29) | (192u << 18) |
                       (1u << 16) | (1u << 14);
    surface_state[1] = kGen9MocsPte << 24;
    surface_state[2] = ((kRenderTargetHeight - 1) << 16) |
                       (kRenderTargetWidth - 1);
    surface_state[3] = kRenderTargetPitch - 1;
    surface_state[7] = (7u << 16) | (6u << 19) |
                       (5u << 22) | (4u << 25);
    surface_state[8] = kRenderTargetPpgtt;
    surface_state[9] = 0;
    static_assert(gen9_probe_fs::kHeapOffset +
                      gen9_probe_fs::kKernelSize <=
                  kPageSize);
    memcpy(reinterpret_cast<uint8_t*>(state_heap) +
               gen9_probe_fs::kHeapOffset,
           gen9_probe_fs::kKernel, gen9_probe_fs::kKernelSize);
    // Dynamic state referenced by kDrawPackets.  The viewport accepts the
    // complete [0,1] depth range; blending is disabled while pre/post color
    // clamps remain enabled for the render-target format.  Color-calc state
    // is all zero because this probe uses neither alpha test nor constants.
    state_heap[gen9_probe_fs::kViewportOffset / sizeof(uint32_t)] = 0;
    state_heap[gen9_probe_fs::kViewportOffset / sizeof(uint32_t) + 1] =
        0x3f800000u;
    state_heap[gen9_probe_fs::kBlendOffset / sizeof(uint32_t)] = 0;
    state_heap[gen9_probe_fs::kBlendOffset / sizeof(uint32_t) + 1] = 0;
    state_heap[gen9_probe_fs::kBlendOffset / sizeof(uint32_t) + 2] = 0xBu;
    // BLORP's no-VS RECTLIST vertices: (48,48), (16,48), (16,16), z=0.
    // The second buffer supplies a zero VUE header and one unused vec4 slot.
    constexpr uint32_t kFloat48 = 0x42400000u;
    constexpr uint32_t kFloat16 = 0x41800000u;
    vertex_data[0] = kFloat48;
    vertex_data[1] = kFloat48;
    vertex_data[2] = 0;
    vertex_data[3] = kFloat16;
    vertex_data[4] = kFloat48;
    vertex_data[5] = 0;
    vertex_data[6] = kFloat16;
    vertex_data[7] = kFloat16;
    vertex_data[8] = 0;
    auto* engine_hwsp_dwords = reinterpret_cast<uint32_t*>(engine_hwsp);
    // Gen8 places six 64-bit context-status entries at DWORD 0x10 and its
    // hardware write pointer at DWORD 0x1f.  Initialize both sides to entry 5
    // so the first event wraps cleanly to entry zero.
    for (size_t i = 0x10; i < 0x10 + 6 * 2; ++i) {
        engine_hwsp_dwords[i] = 0xFFFFFFFFu;
    }
    engine_hwsp_dwords[0x1F] = 5;

    if (!program_ggtt_pages(engine_hwsp_ggtt,
                            g_state.render_engine_hwsp_phys,
                            kRenderEngineHwspSize) ||
        !program_ggtt_pages(context_ggtt,
                            g_state.render_context_phys,
                            kGen9RenderContextSize) ||
        !program_ggtt_pages(ring_ggtt,
                            g_state.render_ring_phys,
                            kRenderRingSize) ||
        !initialize_gen9_render_context(context, ring_ggtt,
                                        g_state.render_ppgtt_roots_phys)) {
        return false;
    }

    flush_cpu_cache(engine_hwsp, kRenderEngineHwspSize);
    flush_cpu_cache(ppgtt_roots, kGen9PpgttRootPages * kPageSize);
    flush_cpu_cache(ppgtt_pt, kPageSize);
    flush_cpu_cache(context, kGen9RenderContextSize);
    flush_cpu_cache(ring, kRenderRingSize);
    flush_cpu_cache(batch, kRenderBatchSize);
    flush_cpu_cache(state_heap, kPageSize);
    flush_cpu_cache(render_target, kRenderTargetSize);
    flush_cpu_cache(vertex_data, kPageSize);
    if (!validate_gen9_context_aperture(context_ggtt, context)) {
        log_message(LogLevel::Warn,
                    "intel-uhd: Gen9 RCS context is not visible through GGTT");
        return false;
    }

    // Gen8+ has a global engine status page in addition to the ppHWSP at the
    // first page of each logical context.  Reset the six-entry Gen8 CSB and
    // then switch RCS from legacy submission into run-list mode.
    mmio_write32(kRenderRingBase + kRingHwstam, 0xFFFFFFFFu);
    mmio_write32(kRenderRingBase + kRingContextStatusPtr, 0xFFFF0505u);
    mmio_write32(kRenderRingBase + kRingModeGen7,
                 masked_enable(kGfxRunListEnable));
    mmio_write32(kRenderRingBase + kRingMiMode,
                 masked_disable(kRingStop));
    mmio_write32(kRenderRingBase + kRingHwsPga,
                 static_cast<uint32_t>(engine_hwsp_ggtt));
    (void)mmio_read32(kRenderRingBase + kRingHwsPga);

    g_state.render_engine_hwsp_ggtt = engine_hwsp_ggtt;
    g_state.render_context_ggtt = context_ggtt;
    g_state.render_ring_ggtt = ring_ggtt;
    g_state.render_batch_gpu_va = kRenderBatchPpgtt;
    g_state.render_context_cpu = context;
    g_state.render_ring_cpu = ring;
    g_state.render_batch_cpu = batch;
    g_state.render_state_heap_cpu = state_heap;
    g_state.render_target_cpu = render_target;
    g_state.render_vertex_cpu = vertex_data;
    g_state.render_ring_tail = 0;
    g_state.render_probe_count = 0;
    g_state.render_context_ready = true;
    log_message(LogLevel::Info,
                "intel-uhd: Gen9 RCS context ready lrc=%016llx ring=%016llx hws=%016llx batch_ppgtt=%08x",
                static_cast<unsigned long long>(context_ggtt),
                static_cast<unsigned long long>(ring_ggtt),
                static_cast<unsigned long long>(engine_hwsp_ggtt),
                static_cast<unsigned int>(kRenderBatchPpgtt));
    return true;
}

bool submit_gen9_render_breadcrumb(uint32_t fence_value) {
    if (!g_state.render_context_ready ||
        g_state.render_context_cpu == nullptr ||
        g_state.render_ring_cpu == nullptr ||
        g_state.render_batch_cpu == nullptr) {
        return false;
    }

    uint32_t tail = g_state.render_ring_tail;
    if (tail > kRenderRingSize - kRenderRequestBytes) {
        // The context is submitted synchronously, so the ring is empty here.
        // Pad through the physical end before placing the next packet at zero;
        // the CS then wraps naturally without executing stale ring contents.
        for (uint32_t offset = tail; offset < kRenderRingSize;
             offset += sizeof(uint32_t)) {
            g_state.render_ring_cpu[offset / sizeof(uint32_t)] = 0;
        }
        flush_cpu_cache(
            const_cast<const uint32_t*>(g_state.render_ring_cpu) +
                tail / sizeof(uint32_t),
            kRenderRingSize - tail);
        tail = 0;
    }

    volatile uint32_t* packet =
        g_state.render_ring_cpu + tail / sizeof(uint32_t);
    const uint64_t fence_gpu_va =
        kRenderFencePpgtt + kRenderProbeFenceOffset;
    // The ring contains only i915's Gen8 request wrapper.  The actual command
    // stream resides in the context's private PPGTT.  Bit 8 selects the
    // non-secure per-process address space used by Gen8+ userspace batches.
    packet[0] = kMiArbOnOff | kMiArbEnable;
    packet[1] = kMiBatchBufferStartGen8 | kMiBatchPpgtt;
    packet[2] = static_cast<uint32_t>(g_state.render_batch_gpu_va);
    packet[3] = static_cast<uint32_t>(g_state.render_batch_gpu_va >> 32);
    // i915's BB_START wrapper disables arbitration here because its later
    // request-finalization tail re-enables it.  Our breadcrumb is inside the
    // indirect batch, so leave arbitration enabled before RCS reaches tail;
    // otherwise the idle context cannot switch out and ELSP stays active.
    packet[4] = kMiArbOnOff | kMiArbEnable;
    packet[5] = 0;
    flush_cpu_cache(const_cast<const uint32_t*>(packet),
                    kRenderRequestBytes);

    emit_gen9_3d_probe_batch(g_state.render_batch_cpu,
                             fence_gpu_va, fence_value);
    flush_cpu_cache(const_cast<const uint32_t*>(g_state.render_batch_cpu),
                    kRenderBatchDwords * sizeof(uint32_t));

    const uint32_t next_tail = tail + kRenderRequestBytes;
    volatile uint32_t* state =
        g_state.render_context_cpu + kPageSize / sizeof(uint32_t);
    // Context retirement saved this cache line from the GPU.  Invalidate the
    // CPU alias before changing RING_TAIL so adjacent saved registers cannot
    // be written back from a stale CPU cache line.
    flush_cpu_cache(const_cast<const uint32_t*>(state + kCtxRingTail),
                    sizeof(uint32_t));
    state[kCtxRingTail] = next_tail;
    flush_cpu_cache(const_cast<const uint32_t*>(state + kCtxRingTail),
                    sizeof(uint32_t));

    auto* fence = reinterpret_cast<volatile uint32_t*>(
        static_cast<uint8_t*>(paging_phys_to_virt(g_state.render_engine_hwsp_phys)) +
        kRenderProbeFenceOffset);
    *fence = 0;
    flush_cpu_cache(const_cast<const uint32_t*>(fence), sizeof(uint32_t));

    const uint64_t descriptor =
        g_state.render_context_ggtt |
        kGen8CtxValid | kGen8CtxForceRestore |
        kGen8CtxPrivilege | kGen8Legacy32BitContext |
        (static_cast<uint64_t>(kGen9ContextId) << 32);
    mmio_write32(kRenderRingBase + kRingElsp, 0);
    mmio_write32(kRenderRingBase + kRingElsp, 0);
    mmio_write32(kRenderRingBase + kRingElsp,
                 static_cast<uint32_t>(descriptor >> 32));
    mmio_write32(kRenderRingBase + kRingElsp,
                 static_cast<uint32_t>(descriptor));
    (void)mmio_read32(kRenderRingBase + kRingExeclistStatusLo);

    const bool fence_complete = wait_for_cpu_value(fence, fence_value);
    // The post-sync write precedes context retirement.  Do not alter the
    // GPU-saved register state or resubmit this context until RCS is idle and
    // the current-context field has cleared.
    const bool ring_idle = wait_for_ring_idle(kRenderRingBase);
    const bool elsp_clear =
        wait_for_mmio_mask(kRenderRingBase + kRingExeclistStatusHi,
                           UINT32_MAX, 0);
    const bool context_retired = fence_complete && ring_idle && elsp_clear;
    asm volatile("lfence" : : : "memory");
    const uint32_t observed_fence = *fence;
    if (!context_retired) {
        const RenderDebugSnapshot debug{
            .flags = (fence_complete ? 1u : 0u) |
                     (ring_idle ? 2u : 0u) |
                     (elsp_clear ? 4u : 0u),
            .head = mmio_read32(kRenderRingBase + kRingHead),
            .tail = mmio_read32(kRenderRingBase + kRingTail),
            .ctl = mmio_read32(kRenderRingBase + kRingCtl),
            .mode = mmio_read32(kRenderRingBase + kRingMiMode),
            .runlist = mmio_read32(kRenderRingBase + kRingModeGen7),
            .els_hi = mmio_read32(kRenderRingBase + kRingExeclistStatusHi),
            .els_lo = mmio_read32(kRenderRingBase + kRingExeclistStatusLo),
            .csb = mmio_read32(kRenderRingBase + kRingContextStatusPtr),
            .fence_xor_expected = observed_fence ^ fence_value,
            .expected = fence_value,
            .eir = mmio_read32(kErrorInterrupt),
            .esr = mmio_read32(kErrorStatus),
            .ipeir = mmio_read32(kRenderIpeir),
            .ipehr = mmio_read32(kRenderIpehr),
            .fault = mmio_read32(kRenderFault),
            .tlb_hi = mmio_read32(kFaultTlbData1),
            .tlb_lo = mmio_read32(kFaultTlbData0),
        };
        char debug_code[192]{};
        if (encode_render_debug(debug, debug_code, sizeof(debug_code)))
            log_message(LogLevel::Warn,
                        "intel-uhd: RCSDBG1 %s", debug_code);
        log_message(LogLevel::Warn,
                    "intel-uhd: Gen9 ELSP submission failed head=%08x tail=%08x ctl=%08x mode=%08x runlist=%08x els=%08x:%08x csb=%08x fence=%08x expected=%08x retired=%u eir=%08x esr=%08x ipe=%08x/%08x fault=%08x tlb=%08x:%08x",
                    static_cast<unsigned int>(debug.head),
                    static_cast<unsigned int>(debug.tail),
                    static_cast<unsigned int>(debug.ctl),
                    static_cast<unsigned int>(debug.mode),
                    static_cast<unsigned int>(debug.runlist),
                    static_cast<unsigned int>(debug.els_hi),
                    static_cast<unsigned int>(debug.els_lo),
                    static_cast<unsigned int>(debug.csb),
                    static_cast<unsigned int>(observed_fence),
                    static_cast<unsigned int>(fence_value),
                    static_cast<unsigned int>(context_retired),
                    static_cast<unsigned int>(debug.eir),
                    static_cast<unsigned int>(debug.esr),
                    static_cast<unsigned int>(debug.ipeir),
                    static_cast<unsigned int>(debug.ipehr),
                    static_cast<unsigned int>(debug.fault),
                    static_cast<unsigned int>(debug.tlb_hi),
                    static_cast<unsigned int>(debug.tlb_lo));
        return false;
    }

    g_state.render_ring_tail = next_tail;
    ++g_state.render_probe_count;
    return true;
}

bool probe_render_engine() {
    g_state.render_target_write_validated = false;
    if (!is_gemini_lake()) {
        log_message(LogLevel::Warn,
                    "intel-uhd: Gen9 ELSP probe is limited to Gemini Lake");
        return false;
    }
    if (!acquire_render_forcewake()) return false;
    if (!setup_render_engine()) {
        release_render_forcewake();
        return false;
    }
    // Submit twice.  The second request starts from the GPU-saved context and
    // proves that tail advancement plus context save/restore is reusable.
    if (!submit_gen9_render_breadcrumb(kRenderProbeFenceValue) ||
        !submit_gen9_render_breadcrumb(kRenderProbeFenceValue + 1u)) {
        mmio_write32(kRenderRingBase + kRingMiMode,
                     masked_enable(kRingStop));
        (void)wait_for_ring_idle(kRenderRingBase);
        mmio_write32(kRenderRingBase + kRingModeGen7,
                     masked_disable(kGfxRunListEnable));
        g_state.render_context_ready = false;
        release_render_forcewake();
        return false;
    }
    flush_cpu_cache(
        const_cast<const uint32_t*>(g_state.render_target_cpu),
        kRenderTargetSize);
    asm volatile("lfence" : : : "memory");
    size_t matching_pixels = 0;
    for (size_t i = 0; i < kRenderTargetSize / sizeof(uint32_t); ++i) {
        if (g_state.render_target_cpu[i] == gen9_probe_fs::kExpectedPixel)
            ++matching_pixels;
    }
    if (matching_pixels == 0) {
        constexpr size_t kCenterPixel =
            (kRenderTargetHeight / 2u) * kRenderTargetWidth +
            (kRenderTargetWidth / 2u);
        log_message(LogLevel::Warn,
                    "intel-uhd: Gen9 render-target write probe failed center=%08x expected=%08x",
                    static_cast<unsigned int>(
                        g_state.render_target_cpu[kCenterPixel]),
                    static_cast<unsigned int>(gen9_probe_fs::kExpectedPixel));
        mmio_write32(kRenderRingBase + kRingMiMode,
                     masked_enable(kRingStop));
        (void)wait_for_ring_idle(kRenderRingBase);
        mmio_write32(kRenderRingBase + kRingModeGen7,
                     masked_disable(kGfxRunListEnable));
        g_state.render_context_ready = false;
        release_render_forcewake();
        return false;
    }
    g_state.render_target_write_validated = true;
    log_message(LogLevel::Info,
                "intel-uhd: Gen9 render-target write probe completed submissions=%u pixels=%zu lrc=%016llx ring=%016llx hws=%016llx batch=%08x target=%08x",
                static_cast<unsigned int>(g_state.render_probe_count),
                matching_pixels,
                static_cast<unsigned long long>(g_state.render_context_ggtt),
                static_cast<unsigned long long>(g_state.render_ring_ggtt),
                static_cast<unsigned long long>(g_state.render_engine_hwsp_ggtt),
                static_cast<unsigned int>(g_state.render_batch_gpu_va),
                static_cast<unsigned int>(kRenderTargetPpgtt));
    release_render_forcewake();
    return true;
}

bool ensure_blt_source_window(size_t page_count) {
    if (page_count == 0 || page_count > SIZE_MAX / kPageSize ||
        g_state.ggtt_table_phys == 0 || !g_state.blt_ready) {
        return false;
    }
    if (g_state.blt_source_ptes != nullptr &&
        page_count <= g_state.blt_source_page_capacity) {
        return true;
    }

    const uint64_t source_ggtt = align_up_u64(
        render_binding_ggtt_base() + kRenderBindingCount * kRenderBindingBytes,
        kPageSize);
    const uint64_t source_bytes = static_cast<uint64_t>(page_count) * kPageSize;
    if (source_ggtt > g_state.aperture_size ||
        source_bytes > g_state.aperture_size - source_ggtt ||
        page_count > SIZE_MAX / sizeof(uint64_t)) {
        return false;
    }
    const uint64_t first_entry = source_ggtt / kPageSize;
    if (first_entry > UINT64_MAX / sizeof(uint64_t) ||
        g_state.ggtt_table_phys > UINT64_MAX - first_entry * sizeof(uint64_t)) {
        return false;
    }
    auto* ptes = reinterpret_cast<volatile uint64_t*>(map_physical_range(
        g_state.ggtt_table_phys + first_entry * sizeof(uint64_t),
        page_count * sizeof(uint64_t),
        PAGE_FLAG_WRITE | PAGE_FLAG_WRITE_THROUGH | PAGE_FLAG_CACHE_DISABLE,
        "BLT source GGTT"));
    if (ptes == nullptr) {
        return false;
    }
    g_state.blt_source_ggtt = source_ggtt;
    g_state.blt_source_ptes = ptes;
    g_state.blt_source_page_capacity = page_count;
    return true;
}

bool bind_render_surface(const render_accel::Surface& surface,
                         uint64_t& out_gpu_va) {
    out_gpu_va = 0;
    if (!g_state.active || !setup_blt_engine() ||
        surface.physical_pages == nullptr || surface.physical_page_count == 0 ||
        surface.byte_length == 0 || surface.byte_length > kRenderBindingBytes ||
        surface.physical_page_count > kRenderBindingBytes / kPageSize) return false;
    size_t slot = kRenderBindingCount;
    for (size_t i = 0; i < kRenderBindingCount; ++i)
        if (!g_render_bindings[i].used) { slot = i; break; }
    if (slot == kRenderBindingCount) return false;
    const uint64_t base = render_binding_ggtt_base() +
                          slot * kRenderBindingBytes;
    if (base > g_state.aperture_size ||
        kRenderBindingBytes > g_state.aperture_size - base ||
        base / kPageSize > UINT64_MAX / sizeof(uint64_t)) return false;
    for (size_t i = 0; i < surface.physical_page_count; ++i) {
        const uint64_t page = surface.physical_pages[i];
        if (page == 0 || (page & (kPageSize - 1)) != 0) return false;
    }
    auto* ptes = reinterpret_cast<volatile uint64_t*>(map_physical_range(
        g_state.ggtt_table_phys + (base / kPageSize) * sizeof(uint64_t),
        surface.physical_page_count * sizeof(uint64_t),
        PAGE_FLAG_WRITE | PAGE_FLAG_WRITE_THROUGH | PAGE_FLAG_CACHE_DISABLE,
        "render GGTT"));
    if (ptes == nullptr) return false;
    for (size_t i = 0; i < surface.physical_page_count; ++i)
        ptes[i] = surface.physical_pages[i] | kGen8GgttPagePresent;
    mmio_write32(kGfxFlushCntlGen6, kGfxFlushCntlEn);
    (void)mmio_read32(kGfxFlushCntlGen6);
    g_render_bindings[slot] = {ptes, surface.physical_page_count, base, true};
    out_gpu_va = base;
    return true;
}

void unbind_render_surface(uint64_t gpu_va) {
    for (size_t i = 0; i < kRenderBindingCount; ++i) {
        RenderBinding& binding = g_render_bindings[i];
        if (!binding.used || binding.gpu_va != gpu_va) continue;
        for (size_t page = 0; page < binding.page_count; ++page) binding.ptes[page] = 0;
        mmio_write32(kGfxFlushCntlGen6, kGfxFlushCntlEn);
        (void)mmio_read32(kGfxFlushCntlGen6);
        binding = {};
        return;
    }
}

bool submit_blt_commands(const uint32_t* commands, size_t dword_count) {
    if (commands == nullptr || dword_count == 0 || g_state.blt_ring_cpu == nullptr) {
        return false;
    }
    const size_t bytes = dword_count * sizeof(uint32_t);
    if (bytes > kBltRingSize) {
        return false;
    }
    if (g_state.blt_disabled || !acquire_blt_forcewake()) {
        g_state.blt_disabled = true;
        log_message(LogLevel::Warn,
                    "intel-uhd: BCS acceleration disabled; using CPU presentation fallback");
        return false;
    }
    if (g_state.blt_ring_tail + bytes > kBltRingSize) {
        if (!wait_for_blt_idle()) {
            // The caller will fall back to CPU presentation/submission.  Do
            // not append new work to a ring whose head has stopped moving.
            mmio_write32(kBltRingBase + kRingMiMode,
                         mmio_read32(kBltRingBase + kRingMiMode) | kRingStop);
            mmio_write32(kBltRingBase + kRingCtl, 0);
            g_state.blt_ready = false;
            g_state.blt_disabled = true;
            release_blt_forcewake();
            return false;
        }
        mmio_write32(kBltRingBase + kRingHead, 0);
        mmio_write32(kBltRingBase + kRingTail, 0);
        g_state.blt_ring_tail = 0;
    }
    for (size_t i = 0; i < dword_count; ++i) {
        g_state.blt_ring_cpu[(g_state.blt_ring_tail / sizeof(uint32_t)) + i] = commands[i];
    }
    asm volatile("mfence" : : : "memory");
    g_state.blt_ring_tail += static_cast<uint32_t>(bytes);
    mmio_write32(kBltRingBase + kRingTail, g_state.blt_ring_tail);
    if (!wait_for_blt_idle()) {
        log_message(LogLevel::Warn, "intel-uhd: BLT command timed out");
        // BCS-only recovery: quiesce the ring, discard its in-flight tail,
        // and let setup_blt_engine reprogram the ring/HWS on the next use.
        // This is intentionally not a global GT reset; render/video engines
        // have not been initialized by this driver and must not be disturbed.
        mmio_write32(kBltRingBase + kRingMiMode,
                     mmio_read32(kBltRingBase + kRingMiMode) | kRingStop);
        mmio_write32(kBltRingBase + kRingCtl, 0);
        (void)mmio_read32(kBltRingBase + kRingCtl);
        mmio_write32(kBltRingBase + kRingHead, 0);
        mmio_write32(kBltRingBase + kRingTail, 0);
        g_state.blt_ring_tail = 0;
        g_state.blt_ready = false;
        g_state.blt_disabled = true;
        ++g_state.blt_reset_count;
        log_message(LogLevel::Warn,
                    "intel-uhd: BCS acceleration disabled after timeout; using CPU presentation fallback");
        release_blt_forcewake();
        return false;
    }
    release_blt_forcewake();
    return true;
}

bool present_framebuffer(const display_accel::Surface& source,
                         uint32_t x,
                         uint32_t y,
                         uint32_t width,
                         uint32_t height) {
    if (!g_state.active || g_state.active_pipe < 0 ||
        g_state.active_pipe >= 3 || source.physical_pages == nullptr ||
        source.bits_per_pixel != 32 || source.width == 0 || source.height == 0 ||
        source.pitch_bytes == 0 || width == 0 || height == 0 ||
        x >= source.width || y >= source.height || width > source.width - x ||
        height > source.height - y || !setup_blt_engine()) {
        return false;
    }

    const PlaneState& plane = g_state.planes[g_state.active_pipe];
    const uint32_t destination_pitch =
        (plane.stride & PLANE_STRIDE_MASK) * PLANE_STRIDE_GRANULARITY;
    if (source.width != plane.width || source.height != plane.height ||
        source.pitch_bytes != destination_pitch || x >= plane.width ||
        y >= plane.height || width > plane.width - x || height > plane.height - y ||
        source.pitch_bytes > 0xFFFFu ||
        source.height > static_cast<size_t>(-1) / source.pitch_bytes ||
        source.byte_length < static_cast<size_t>(source.pitch_bytes) * source.height ||
        source.byte_length > SIZE_MAX - (kPageSize - 1)) {
        return false;
    }
    const size_t required_pages =
        (source.byte_length + kPageSize - 1) / kPageSize;
    if (required_pages == 0 || required_pages > source.physical_page_count ||
        !ensure_blt_source_window(required_pages)) {
        return false;
    }
    for (size_t i = 0; i < required_pages; ++i) {
        const uint64_t page = source.physical_pages[i];
        if ((page & (kPageSize - 1)) != 0 || page == 0) {
            return false;
        }
        g_state.blt_source_ptes[i] = page | kGen8GgttPagePresent;
    }
    mmio_write32(kGfxFlushCntlGen6, kGfxFlushCntlEn);
    (void)mmio_read32(kGfxFlushCntlGen6);

    uint32_t commands[10]{};
    commands[0] = kXySrcCopyBltCmd | kBltWriteRgba | ((8 - 2) + 2);
    commands[1] = kBltDepth32 | kBltRopSrcCopy | source.pitch_bytes;
    commands[2] = (y << 16) | x;
    commands[3] = ((y + height) << 16) | (x + width);
    commands[4] = static_cast<uint32_t>(g_state.scanout_ggtt_offset & 0xFFFFFFFFu);
    commands[5] = static_cast<uint32_t>(g_state.scanout_ggtt_offset >> 32);
    commands[6] = (y << 16) | x;
    commands[7] = source.pitch_bytes;
    commands[8] = static_cast<uint32_t>(g_state.blt_source_ggtt & 0xFFFFFFFFu);
    commands[9] = static_cast<uint32_t>(g_state.blt_source_ggtt >> 32);
    return submit_blt_commands(commands, sizeof(commands) / sizeof(commands[0]));
}

bool fill_framebuffer(const display_accel::Surface& target,
                      uint32_t x,
                      uint32_t y,
                      uint32_t width,
                      uint32_t height,
                      uint32_t color) {
    if (!g_state.active || g_state.active_pipe < 0 || g_state.active_pipe >= 3 ||
        target.physical_pages == nullptr || target.bits_per_pixel != 32 ||
        target.pitch_bytes == 0 || target.width == 0 || target.height == 0 ||
        width == 0 || height == 0 || x >= target.width || y >= target.height ||
        width > target.width - x || height > target.height - y ||
        target.pitch_bytes > 0xFFFFu || !setup_blt_engine()) {
        return false;
    }
    const PlaneState& plane = g_state.planes[g_state.active_pipe];
    const uint32_t pitch =
        (plane.stride & PLANE_STRIDE_MASK) * PLANE_STRIDE_GRANULARITY;
    if (target.width != plane.width || target.height != plane.height ||
        target.pitch_bytes != pitch || target.height > SIZE_MAX / target.pitch_bytes ||
        target.byte_length < static_cast<size_t>(target.pitch_bytes) * target.height ||
        target.byte_length > SIZE_MAX - (kPageSize - 1)) {
        return false;
    }
    const size_t pages = (target.byte_length + kPageSize - 1) / kPageSize;
    if (pages == 0 || pages > target.physical_page_count ||
        !ensure_blt_source_window(pages)) {
        return false;
    }
    for (size_t i = 0; i < pages; ++i) {
        const uint64_t page = target.physical_pages[i];
        if (page == 0 || (page & (kPageSize - 1)) != 0) return false;
        g_state.blt_source_ptes[i] = page | kGen8GgttPagePresent;
    }
    mmio_write32(kGfxFlushCntlGen6, kGfxFlushCntlEn);
    (void)mmio_read32(kGfxFlushCntlGen6);
    uint32_t commands[7]{};
    commands[0] = kXyColorBltCmd | kBltWriteRgba | (7 - 2);
    commands[1] = kBltDepth32 | kBltRopSrcCopy | target.pitch_bytes;
    commands[2] = (y << 16) | x;
    commands[3] = ((y + height) << 16) | (x + width);
    commands[4] = static_cast<uint32_t>(g_state.blt_source_ggtt & 0xFFFFFFFFu);
    commands[5] = static_cast<uint32_t>(g_state.blt_source_ggtt >> 32);
    commands[6] = color;
    return submit_blt_commands(commands, sizeof(commands) / sizeof(commands[0]));
}

// This is a deliberately restricted render-node operation.  The kernel has
// already validated the byte range and owns all GGTT mappings; userspace never
// supplies a GPU batch or GPU virtual address.
bool render_fill(const render_accel::Surface& target,
                 uint64_t gpu_va,
                 uint64_t byte_offset,
                 uint32_t pitch_bytes,
                 uint32_t x,
                 uint32_t y,
                 uint32_t width,
                 uint32_t height,
                 uint32_t color) {
    if (!g_state.active || target.physical_pages == nullptr ||
        target.physical_page_count == 0 || gpu_va == 0 ||
        byte_offset > target.byte_length || (byte_offset & (kPageSize - 1)) != 0 ||
        pitch_bytes == 0 || pitch_bytes > 0xFFFFu || width == 0 || height == 0 ||
        x > pitch_bytes / 4 || width > pitch_bytes / 4 - x ||
        y > 0xFFFFu || height > 0xFFFFu - y ||
        !setup_blt_engine()) return false;
    const size_t pages = (target.byte_length + kPageSize - 1) / kPageSize;
    if (pages == 0 || pages > target.physical_page_count ||
        gpu_va > g_state.aperture_size ||
        target.byte_length > g_state.aperture_size - gpu_va) return false;
    mmio_write32(kGfxFlushCntlGen6, kGfxFlushCntlEn);
    (void)mmio_read32(kGfxFlushCntlGen6);
    uint32_t commands[7]{};
    commands[0] = kXyColorBltCmd | kBltWriteRgba | (7 - 2);
    commands[1] = kBltDepth32 | kBltRopSrcCopy | pitch_bytes;
    commands[2] = (y << 16) | x;
    commands[3] = ((y + height) << 16) | (x + width);
    const uint64_t destination = gpu_va + byte_offset;
    commands[4] = static_cast<uint32_t>(destination & 0xFFFFFFFFu);
    commands[5] = static_cast<uint32_t>(destination >> 32);
    commands[6] = color;
    return submit_blt_commands(commands, sizeof(commands) / sizeof(commands[0]));
}

bool sync_render_surface(const render_accel::Surface& surface,
                         uint64_t byte_offset,
                         uint64_t byte_length,
                         uint32_t flags) {
    constexpr uint32_t kValidFlags =
        neutrino_render::kBufferSyncCpuToDevice |
        neutrino_render::kBufferSyncDeviceToCpu;
    if (surface.physical_pages == nullptr ||
        surface.physical_page_count == 0 || surface.byte_length == 0 ||
        flags == 0 || (flags & ~kValidFlags) != 0 || byte_length == 0 ||
        byte_offset > surface.byte_length ||
        byte_length > surface.byte_length - byte_offset)
        return false;

    uint64_t cursor = byte_offset;
    uint64_t remaining = byte_length;
    while (remaining != 0) {
        const size_t page_index = static_cast<size_t>(cursor / kPageSize);
        const size_t in_page = static_cast<size_t>(cursor % kPageSize);
        if (page_index >= surface.physical_page_count) return false;
        const uint64_t physical = surface.physical_pages[page_index];
        if (physical == 0 || (physical & (kPageSize - 1)) != 0) return false;
        size_t chunk = kPageSize - in_page;
        if (remaining < chunk) chunk = static_cast<size_t>(remaining);
        const auto* address = static_cast<const uint8_t*>(
                                  paging_phys_to_virt(physical)) +
                              in_page;
        flush_cpu_cache(address, chunk);
        cursor += chunk;
        remaining -= chunk;
    }
    return true;
}

// Execute the same bounded draw used during bring-up and return its pixels in
// a render-descriptor BO.  RCS continues to reference only permanent
// kernel-owned pages; after retirement the CPU copies the verified-size image
// into the caller's pinned pages.  This staging copy is deliberate: a failed
// request can never leave a hung GPU holding references to reclaimable
// userspace memory.
bool render_draw_demo(const render_accel::Surface& target,
                      uint32_t pitch_bytes,
                      uint32_t width,
                      uint32_t height) {
    if (!g_state.render_target_write_validated ||
        !g_state.render_context_ready ||
        target.physical_pages == nullptr ||
        target.physical_page_count < kRenderTargetPages ||
        target.byte_length < kRenderTargetSize ||
        pitch_bytes != kRenderTargetPitch ||
        width != kRenderTargetWidth || height != kRenderTargetHeight ||
        g_state.render_target_cpu == nullptr)
        return false;

    for (size_t page = 0; page < kRenderTargetPages; ++page) {
        const uint64_t physical = target.physical_pages[page];
        if (physical == 0 || (physical & (kPageSize - 1)) != 0)
            return false;
    }
    if (!acquire_render_forcewake()) return false;
    memset(const_cast<uint32_t*>(g_state.render_target_cpu), 0,
           kRenderTargetSize);
    flush_cpu_cache(const_cast<const uint32_t*>(g_state.render_target_cpu),
                    kRenderTargetSize);
    const uint32_t fence = kRenderProbeFenceValue +
                           g_state.render_probe_count + 1u;
    const bool submitted = submit_gen9_render_breadcrumb(fence);
    size_t matching_pixels = 0;
    if (submitted) {
        flush_cpu_cache(
            const_cast<const uint32_t*>(g_state.render_target_cpu),
            kRenderTargetSize);
        for (size_t i = 0; i < kRenderTargetSize / sizeof(uint32_t); ++i)
            if (g_state.render_target_cpu[i] ==
                gen9_probe_fs::kExpectedPixel)
                ++matching_pixels;
    }
    const bool rendered = submitted && matching_pixels != 0;
    if (rendered) {
        for (size_t page = 0; page < kRenderTargetPages; ++page) {
            void* destination =
                paging_phys_to_virt(target.physical_pages[page]);
            memcpy(destination,
                   const_cast<const uint32_t*>(g_state.render_target_cpu) +
                       page * kPageSize / sizeof(uint32_t),
                   kPageSize);
            flush_cpu_cache(destination, kPageSize);
        }
        log_message(LogLevel::Info,
                    "intel-uhd: userspace 3D demo draw completed submission=%u pixels=%zu",
                    static_cast<unsigned int>(g_state.render_probe_count),
                    matching_pixels);
    } else {
        g_state.render_target_write_validated = false;
        log_message(LogLevel::Warn,
                    "intel-uhd: userspace 3D demo draw failed submitted=%u pixels=%zu",
                    submitted ? 1u : 0u, matching_pixels);
    }
    release_render_forcewake();
    return rendered;
}

uint16_t decode_active_dimension(uint32_t reg_value) {
    return static_cast<uint16_t>(((reg_value >> 16) & 0x1FFFu) + 1u);
}

uint16_t decode_total_dimension(uint32_t reg_value) {
    return static_cast<uint16_t>((reg_value & 0x1FFFu) + 1u);
}

char pipe_name(size_t index) {
    return static_cast<char>('A' + index);
}

uint16_t decode_plane_dimension(uint32_t reg_value, uint32_t shift) {
    return static_cast<uint16_t>(((reg_value >> shift) & 0x0FFFu) + 1u);
}

void capture_pipe_state(size_t pipe_index) {
    if (pipe_index >= 3) {
        return;
    }

    PipeState& pipe = g_state.pipes[pipe_index];
    uint32_t stride = static_cast<uint32_t>(pipe_index) * PIPE_STRIDE;
    pipe.htotal = mmio_read32(TRANS_HTOTAL_A + stride);
    pipe.vtotal = mmio_read32(TRANS_VTOTAL_A + stride);
    pipe.source = mmio_read32(PIPE_SRC_A + stride);
    pipe.pipeconf = mmio_read32(PIPECONF_A + stride);
    pipe.ddi_func_ctl = mmio_read32(TRANS_DDI_FUNC_CTL_A + stride);
    pipe.enabled = ((pipe.pipeconf & PIPECONF_ENABLE) != 0) ||
                   ((pipe.ddi_func_ctl & DDI_FUNC_ENABLE) != 0);
}

void capture_plane_state(size_t pipe_index) {
    if (pipe_index >= 3) {
        return;
    }

    PlaneState& plane = g_state.planes[pipe_index];
    uint32_t stride = static_cast<uint32_t>(pipe_index) * PIPE_STRIDE;
    plane.ctl = mmio_read32(PLANE_CTL_1_A + stride);
    plane.stride = mmio_read32(PLANE_STRIDE_1_A + stride);
    plane.size = mmio_read32(PLANE_SIZE_1_A + stride);
    plane.surf = mmio_read32(PLANE_SURF_1_A + stride);
    plane.surf_live = mmio_read32(PLANE_SURFLIVE_1_A + stride);
    plane.offset = mmio_read32(PLANE_OFFSET_1_A + stride);
    plane.enabled = (plane.ctl & PLANE_CTL_ENABLE) != 0;
    plane.linear = (plane.ctl & PLANE_CTL_TILED_MASK) == PLANE_CTL_TILED_LINEAR;
    plane.xrgb8888 =
        (plane.ctl & PLANE_CTL_FORMAT_MASK) == PLANE_CTL_FORMAT_XRGB_8888;
    plane.rgbx_order = (plane.ctl & PLANE_CTL_ORDER_RGBX) != 0;
    plane.width = decode_plane_dimension(plane.size, 0);
    plane.height = decode_plane_dimension(plane.size, 16);
    plane.offset_x = static_cast<uint16_t>(plane.offset & 0xFFFFu);
    plane.offset_y = static_cast<uint16_t>((plane.offset >> 16) & 0xFFFFu);
}

uint32_t plane_stride_bytes(const PlaneState& plane) {
    return (plane.stride & PLANE_STRIDE_MASK) * PLANE_STRIDE_GRANULARITY;
}

void log_pipe_state(size_t pipe_index) {
    if (pipe_index >= 3) {
        return;
    }

    const PipeState& pipe = g_state.pipes[pipe_index];
    if (!pipe.enabled) {
        log_message(LogLevel::Info, "intel-uhd: pipe %c disabled",
                    static_cast<unsigned int>(pipe_name(pipe_index)));
        return;
    }

    uint16_t width = decode_active_dimension(pipe.source);
    uint16_t height = static_cast<uint16_t>((pipe.source & 0x1FFFu) + 1u);
    uint16_t htotal = decode_total_dimension(pipe.htotal);
    uint16_t hactive = decode_active_dimension(pipe.htotal);
    uint16_t vtotal = decode_total_dimension(pipe.vtotal);
    uint16_t vactive = decode_active_dimension(pipe.vtotal);

    log_message(
        LogLevel::Info,
        "intel-uhd: pipe %c active mode=%ux%u hactive=%u htotal=%u vactive=%u vtotal=%u pipeconf=%08x ddi=%08x",
        static_cast<unsigned int>(pipe_name(pipe_index)),
        static_cast<unsigned int>(width),
        static_cast<unsigned int>(height),
        static_cast<unsigned int>(hactive),
        static_cast<unsigned int>(htotal),
        static_cast<unsigned int>(vactive),
        static_cast<unsigned int>(vtotal),
        static_cast<unsigned int>(pipe.pipeconf),
        static_cast<unsigned int>(pipe.ddi_func_ctl));
}

bool bind_current_scanout() {
    if (g_state.aperture_phys == 0) {
        log_message(LogLevel::Warn, "intel-uhd: missing BAR2 aperture");
        return false;
    }

    const size_t expected_width = 0;
    const size_t expected_height = 0;
    char failure_summary[256];
    size_t failure_used = 0;
    failure_summary[0] = '\0';

    auto append_summary = [&](const char* text) {
        if (text == nullptr || failure_used + 1 >= sizeof(failure_summary)) {
            return;
        }
        while (*text != '\0' && failure_used + 1 < sizeof(failure_summary)) {
            failure_summary[failure_used++] = *text++;
        }
        failure_summary[failure_used] = '\0';
    };

    auto append_char = [&](char ch) {
        if (failure_used + 1 >= sizeof(failure_summary)) {
            return;
        }
        failure_summary[failure_used++] = ch;
        failure_summary[failure_used] = '\0';
    };

    auto append_u32 = [&](uint32_t value) {
        char digits[10];
        size_t count = 0;
        if (value == 0) {
            append_char('0');
            return;
        }
        while (value != 0 && count < sizeof(digits)) {
            digits[count++] = static_cast<char>('0' + (value % 10u));
            value /= 10u;
        }
        while (count > 0) {
            append_char(digits[--count]);
        }
    };

    auto append_pipe_summary = [&](size_t pipe,
                                   const PlaneState& plane,
                                   const PipeState& pipe_state,
                                   const char* reason) {
        const char pipe_char = pipe_name(pipe);
        uint32_t stride_bytes = plane_stride_bytes(plane);
        if (reason == nullptr) {
            return;
        }
        append_char(pipe_char);
        append_summary(":e");
        append_u32(plane.enabled ? 1u : 0u);
        append_summary("/p");
        append_u32(pipe_state.enabled ? 1u : 0u);
        append_summary("/l");
        append_u32(plane.linear ? 1u : 0u);
        append_summary("/x");
        append_u32(plane.xrgb8888 ? 1u : 0u);
        append_char('/');
        append_u32(plane.width);
        append_char('x');
        append_u32(plane.height);
        append_summary("/s");
        append_u32(stride_bytes);
        append_char(':');
        append_summary(reason);
        append_char(' ');
    };

    for (size_t pipe = 0; pipe < 3; ++pipe) {
        const PipeState& pipe_state = g_state.pipes[pipe];
        const PlaneState& plane = g_state.planes[pipe];
        if (!plane.enabled) {
            append_pipe_summary(pipe, plane, pipe_state, "off");
            continue;
        }
        if (!plane.linear || !plane.xrgb8888) {
            append_pipe_summary(pipe, plane, pipe_state, "fmt");
            continue;
        }

        uint32_t stride_bytes = plane_stride_bytes(plane);
        if (stride_bytes == 0 || plane.width == 0 || plane.height == 0) {
            append_pipe_summary(pipe, plane, pipe_state, "geom");
            continue;
        }
        if (expected_width != 0 && expected_height != 0 &&
            (plane.width != expected_width ||
             plane.height != expected_height)) {
            append_pipe_summary(pipe, plane, pipe_state, "size");
            continue;
        }
        if (plane.width > UINT32_MAX / 4u ||
            stride_bytes < plane.width * 4u) {
            append_pipe_summary(pipe, plane, pipe_state, "stride");
            continue;
        }

        uint32_t surf_reg = (plane.surf_live != 0) ? plane.surf_live : plane.surf;
        uint64_t ggtt_offset = static_cast<uint64_t>(surf_reg & 0xFFFFF000u);
        uint64_t y_offset =
            static_cast<uint64_t>(plane.offset_y) * stride_bytes;
        uint64_t x_offset = static_cast<uint64_t>(plane.offset_x) * 4u;
        if (y_offset > UINT64_MAX - ggtt_offset ||
            x_offset > UINT64_MAX - (ggtt_offset + y_offset)) {
            append_pipe_summary(pipe, plane, pipe_state, "offset");
            continue;
        }
        ggtt_offset += y_offset + x_offset;

        if (plane.height > static_cast<size_t>(-1) / stride_bytes) {
            append_pipe_summary(pipe, plane, pipe_state, "bytes");
            continue;
        }
        size_t frame_bytes = static_cast<size_t>(stride_bytes) * plane.height;
        if (g_state.aperture_phys == 0 || g_state.aperture_size == 0 ||
            ggtt_offset > g_state.aperture_size ||
            frame_bytes > g_state.aperture_size - ggtt_offset ||
            ggtt_offset > UINT64_MAX - g_state.aperture_phys) {
            append_pipe_summary(pipe, plane, pipe_state, "aperture");
            continue;
        }
        uint64_t phys = g_state.aperture_phys + ggtt_offset;
        uint8_t* base = map_physical_range(phys, frame_bytes, PAGE_FLAG_WRITE, "scanout");
        if (base == nullptr) {
            return false;
        }
        if (!paging_mark_wc(reinterpret_cast<uint64_t>(base), frame_bytes)) {
            log_message(LogLevel::Warn,
                        "intel-uhd: failed to mark scanout WC (virt=%016llx len=%llu)",
                        reinterpret_cast<unsigned long long>(base),
                        static_cast<unsigned long long>(frame_bytes));
        }

        Framebuffer fb{};
        fb.base = base;
        fb.width = plane.width;
        fb.height = plane.height;
        fb.pitch = stride_bytes;
        fb.bpp = 32;
        if (plane.rgbx_order) {
            fb.red_mask_size = 8;
            fb.red_mask_shift = 0;
            fb.green_mask_size = 8;
            fb.green_mask_shift = 8;
            fb.blue_mask_size = 8;
            fb.blue_mask_shift = 16;
        } else {
            fb.red_mask_size = 8;
            fb.red_mask_shift = 16;
            fb.green_mask_size = 8;
            fb.green_mask_shift = 8;
            fb.blue_mask_size = 8;
            fb.blue_mask_shift = 0;
        }
        descriptor::register_framebuffer_device(fb, phys);
        if (kconsole != nullptr) {
            kconsole->present();
        }

        g_state.scanout_base = base;
        g_state.scanout_phys = phys;
        g_state.scanout_ggtt_offset = ggtt_offset;
        g_state.scanout_bytes = frame_bytes;
        g_state.active_pipe = static_cast<int>(pipe);

        log_message(
            LogLevel::Info,
            "intel-uhd: adopted pipe %c scanout base=%016llx size=%zu stride=%u",
            static_cast<unsigned int>(pipe_name(pipe)),
            static_cast<unsigned long long>(phys),
            frame_bytes,
            static_cast<unsigned int>(stride_bytes));
        if (!pipe_state.enabled) {
            log_message(LogLevel::Warn,
                        "intel-uhd: adopted plane %c.1 even though pipe %c did not report active",
                        static_cast<unsigned int>(pipe_name(pipe)),
                        static_cast<unsigned int>(pipe_name(pipe)));
        }
        return true;
    }

    log_message(LogLevel::Warn,
                "intel-uhd: no adoptable primary plane found expected=%zux%zu %s",
                expected_width,
                expected_height,
                failure_summary);
    return false;
}

void log_device_state() {
    uint16_t gmch = pci::read_config16(g_state.device, REG_GMCH_GMS);
    uint32_t gfx_mode = mmio_read32(REG_GFX_MODE);
    uint32_t ggtt_pat = mmio_read32(REG_GGTT_PAT);
    uint32_t cdclk = mmio_read32(REG_CDCLK_CTL);

    log_message(
        LogLevel::Info,
        "intel-uhd: online at %02u:%02u.%u device=%04x (%s) mmio=%016llx aperture=%016llx cmd=%04x gmch=%04x gfx_mode=%08x ggtt_pat=%08x cdclk=%08x",
        static_cast<unsigned int>(g_state.device.bus),
        static_cast<unsigned int>(g_state.device.slot),
        static_cast<unsigned int>(g_state.device.function),
        static_cast<unsigned int>(g_state.device.device),
        (g_state.info != nullptr) ? g_state.info->name : "unknown",
        static_cast<unsigned long long>(g_state.mmio_phys),
        static_cast<unsigned long long>(g_state.aperture_phys),
        static_cast<unsigned int>(g_state.command),
        static_cast<unsigned int>(gmch),
        static_cast<unsigned int>(gfx_mode),
        static_cast<unsigned int>(ggtt_pat),
        static_cast<unsigned int>(cdclk));

    for (size_t i = 0; i < 3; ++i) {
        log_pipe_state(i);
    }
}

bool init_device(const pci::PciDevice& device) {
    const DeviceInfo* info = lookup_device_info(device.device);
    if (info == nullptr) {
        return false;
    }

    DriverState next{};
    next.initialized = true;
    next.device = device;
    next.info = info;
    next.mmio_phys = pci_bar_base(device, 0);
    next.aperture_phys = pci_bar_base(device, 2);
    next.mmio_size = pci_bar_size(device, 0);
    next.aperture_size = pci_bar_size(device, 2);
    if (next.mmio_phys == 0) {
        log_message(LogLevel::Warn,
                    "intel-uhd: missing BAR0 on %02u:%02u.%u",
                    static_cast<unsigned int>(device.bus),
                    static_cast<unsigned int>(device.slot),
                    static_cast<unsigned int>(device.function));
        return false;
    }

    uint16_t command = pci::read_config16(device, 0x04);
    command |= static_cast<uint16_t>(kPciCommandMemory | kPciCommandBusMaster);
    command &= static_cast<uint16_t>(~kPciCommandIo);
    pci::write_config16(device, 0x04, command);
    next.command = pci::read_config16(device, 0x04);

    next.regs = map_bar_window(device, 0, kMmioMapSize);
    if (next.regs == nullptr) {
        log_message(LogLevel::Warn,
                    "intel-uhd: failed to map BAR0 for %02u:%02u.%u",
                    static_cast<unsigned int>(device.bus),
                    static_cast<unsigned int>(device.slot),
                    static_cast<unsigned int>(device.function));
        return false;
    }

    if (next.mmio_size > kGgttTableOffset && next.aperture_size != 0) {
        const uint64_t ggtt_bytes =
            (next.aperture_size / kPageSize) * sizeof(uint64_t);
        if (ggtt_bytes <= next.mmio_size - kGgttTableOffset) {
            next.ggtt_table_phys = next.mmio_phys + kGgttTableOffset;
        }
    }

    g_state = next;
    for (size_t i = 0; i < 3; ++i) {
        capture_pipe_state(i);
        capture_plane_state(i);
    }

    (void)bind_current_scanout();
    g_state.active = true;
    if (!register_framebuffer_acceleration()) {
        log_message(LogLevel::Info,
                    "intel-uhd: framebuffer acceleration unavailable; using CPU presentation");
    }
    if (g_render_probe_requested && !probe_render_engine()) {
        log_message(LogLevel::Warn,
                    "intel-uhd: RCS probe failed; BLT and display paths remain enabled");
    }
    log_device_state();
    return true;
}

}  // namespace

void configure() {
    g_disabled =
        kernel_cmdline::has_value("INTEL_UHD", "OFF") ||
        kernel_cmdline::has_flag("INTEL_UHD.DISABLE");
    g_render_probe_requested = kernel_cmdline::has_flag("INTEL_UHD.RCS_PROBE");
}

void register_driver() {
    if (g_disabled) {
        log_message(LogLevel::Info,
                    "intel-uhd: disabled by kernel command line");
        return;
    }
#ifdef NEUTRINO_DYNAMIC_MODULE_INTEL_UHD
    if (g_module_api == nullptr || g_module_api->register_pci_driver == nullptr) {
        return;
    }
    (void)g_module_api->register_pci_driver(
        "intel-uhd",
        kPciMatches,
        sizeof(kPciMatches) / sizeof(kPciMatches[0]),
        init);
#else
    (void)driver_registry::register_pci_driver(
        "intel-uhd",
        kPciMatches,
        sizeof(kPciMatches) / sizeof(kPciMatches[0]),
        init);
#endif
}

bool register_module() {
    configure();
    register_driver();
    return true;
}

#ifndef NEUTRINO_DYNAMIC_MODULE_INTEL_UHD
KERNEL_BUILTIN_MODULE(intel_uhd_module,
                      "intel-uhd",
                      kernel_module::Phase::Driver,
                      register_module,
                      kPciMatches,
                      sizeof(kPciMatches) / sizeof(kPciMatches[0]));
#else
KERNEL_DYNAMIC_MODULE_DESCRIPTOR(
    "intel-uhd-gemini-lake",
    kernel_module::Phase::Driver,
    kPciMatches,
    sizeof(kPciMatches) / sizeof(kPciMatches[0]));

extern "C" bool neutrino_module_init(const kernel_module::Api* api) {
    if (api == nullptr ||
        api->abi_version != kernel_module::kDescriptorAbiVersion) {
        return false;
    }
    g_module_api = api;
    return register_module();
}
#endif

void init() {
    if (g_state.initialized || g_disabled) {
        return;
    }

    g_state.initialized = true;

    const pci::PciDevice* list = pci::devices();
    size_t count = pci::device_count();
    for (size_t i = 0; i < count; ++i) {
        const pci::PciDevice& device = list[i];
        if (device.vendor != kIntelVendorId || device.class_code != 0x03) {
            continue;
        }

        const DeviceInfo* info = lookup_device_info(device.device);
        if (info == nullptr) {
            log_message(LogLevel::Info,
                        "intel-uhd: skipping unsupported Intel graphics device %04x at %02u:%02u.%u",
                        static_cast<unsigned int>(device.device),
                        static_cast<unsigned int>(device.bus),
                        static_cast<unsigned int>(device.slot),
                        static_cast<unsigned int>(device.function));
            continue;
        }

        if (init_device(device)) {
            return;
        }
    }

    log_message(LogLevel::Info, "intel-uhd: no supported device found");
}

bool available() {
    return g_state.active;
}

bool blit_copy(unsigned int src_x,
               unsigned int src_y,
               unsigned int dst_x,
               unsigned int dst_y,
               unsigned int width,
               unsigned int height,
               unsigned int pitch_bytes) {
    if (!g_state.active || width == 0 || height == 0 || pitch_bytes == 0) {
        return false;
    }
    if (src_x == dst_x && src_y == dst_y) {
        return true;
    }
    if (!setup_blt_engine()) {
        return false;
    }

    const uint32_t max_x = static_cast<uint32_t>(g_state.planes[g_state.active_pipe].width);
    const uint32_t max_y = static_cast<uint32_t>(g_state.planes[g_state.active_pipe].height);
    if (src_x + width > max_x || dst_x + width > max_x ||
        src_y + height > max_y || dst_y + height > max_y) {
        return false;
    }

    uint32_t commands[10]{};
    commands[0] = kXySrcCopyBltCmd | kBltWriteRgba | ((8 - 2) + 2);
    commands[1] = kBltDepth32 | kBltRopSrcCopy | pitch_bytes;
    commands[2] = (dst_y << 16) | dst_x;
    commands[3] = ((dst_y + height) << 16) | (dst_x + width);
    commands[4] = static_cast<uint32_t>(g_state.scanout_ggtt_offset & 0xFFFFFFFFu);
    commands[5] = static_cast<uint32_t>(g_state.scanout_ggtt_offset >> 32);
    commands[6] = (src_y << 16) | src_x;
    commands[7] = pitch_bytes;
    commands[8] = static_cast<uint32_t>(g_state.scanout_ggtt_offset & 0xFFFFFFFFu);
    commands[9] = static_cast<uint32_t>(g_state.scanout_ggtt_offset >> 32);
    return submit_blt_commands(commands, sizeof(commands) / sizeof(commands[0]));
}

bool register_framebuffer_acceleration() {
    if (!g_state.active || g_disabled || g_state.active_pipe < 0 ||
        g_state.active_pipe >= 3) {
        return false;
    }
    const display_accel::Ops ops{
        .present = present_framebuffer,
        .fill = fill_framebuffer,
    };
    const render_accel::Ops render_ops{
        .fill = render_fill,
        .bind = bind_render_surface,
        .unbind = unbind_render_surface,
        .query_device_info = [](render_accel::DeviceInfo& info) {
            if (!g_state.active) return false;
            info = {
                .vendor_id = kIntelVendorId,
                .device_id = g_state.device.device,
                .graphics_version = is_gemini_lake()
                                        ? static_cast<uint16_t>(9)
                                        : static_cast<uint16_t>(0),
                .graphics_version_minor = is_gemini_lake()
                                              ? static_cast<uint16_t>(1)
                                              : static_cast<uint16_t>(0),
                .engines = neutrino_render::kEngineBlt |
                           (is_gemini_lake()
                                ? neutrino_render::kEngineRender
                                : 0u),
                .capabilities = neutrino_render::kCapabilityGpuVa |
                    (g_state.render_sync_registered
                         ? neutrino_render::kCapabilityExplicitCacheSync
                         : 0u) |
                    (g_state.render_target_write_validated
                         ? neutrino_render::kCapabilityExeclists |
                               neutrino_render::kCapabilityPpgtt32 |
                               neutrino_render::kCapability3dPipeline |
                               neutrino_render::kCapabilityStateBaseAddress |
                               neutrino_render::kCapabilityFragmentShader |
                               neutrino_render::kCapabilityRenderTargetWrite |
                               (g_state.render_demo_registered
                                    ? neutrino_render::kCapabilityDemoDraw
                                    : 0u)
                         : 0u),
            };
            return true;
        },
    };
    const bool display_registered = neutrino_register_framebuffer_presenter(&ops);
    const bool render_registered = neutrino_register_render_accelerator(&render_ops);
    const bool sync_registered = render_registered &&
        neutrino_register_render_sync_accelerator(sync_render_surface);
    g_state.render_sync_registered = sync_registered;
    const bool demo_registered = render_registered &&
        neutrino_register_render_demo_accelerator(render_draw_demo);
    g_state.render_demo_registered = demo_registered;
    return display_registered || render_registered;
}

}  // namespace intel_uhd
