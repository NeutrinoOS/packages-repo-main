#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../crt/syscall.hpp"

namespace service_protocol {

constexpr uint32_t kRegistryMagic = 0x53564352u;  // "SVCR"
constexpr uint32_t kRegistryVersion = 1;
constexpr char kRegistryName[] = "init.service.registry";

constexpr uint32_t kMessageMagic = 0x5356434Du;  // "SVCM"
constexpr uint16_t kMessageVersion = 1;
constexpr uint32_t kAuthorizationEvent = 0x53564301u;
constexpr size_t kNameSize = 32;
constexpr size_t kResponseTextSize = 12288;

enum Command : uint16_t {
    kList = 1,
    kStatus = 2,
    kStart = 3,
    kStop = 4,
    kRestart = 5,
    kLogs = 6,
};

enum Status : int32_t {
    kStatusOk = 0,
    kStatusInvalid = -1,
    kStatusNotFound = -2,
    kStatusFailed = -3,
};

struct Registry {
    uint32_t magic;
    uint32_t version;
    uint32_t server_pipe_id;
    uint32_t manager_process_id;
};

struct Request {
    uint32_t magic;
    uint16_t version;
    uint16_t command;
    uint32_t reply_pipe_id;
    uint32_t client_process_id;
    char service[kNameSize];
};

struct ResponseHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    int32_t status;
    uint32_t text_length;
};

inline bool write_all(uint32_t handle, const void* data, size_t length) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    size_t written = 0;
    descriptor_defs::DescriptorWait wait{};
    wait.handle = handle;
    wait.events = descriptor_defs::kWaitWrite;
    while (written < length) {
        long result = descriptor_write(handle, bytes + written, length - written);
        if (result == kDescriptorWouldBlock) {
            wait.revents = 0;
            if (descriptor_wait(&wait, 1) < 0) {
                yield();
            }
            continue;
        }
        if (result <= 0) {
            return false;
        }
        written += static_cast<size_t>(result);
    }
    return true;
}

inline bool read_all(uint32_t handle, void* data, size_t length, bool wait_first) {
    auto* bytes = static_cast<uint8_t*>(data);
    size_t total = 0;
    descriptor_defs::DescriptorWait wait{};
    wait.handle = handle;
    wait.events = descriptor_defs::kWaitRead;
    while (total < length) {
        long result = descriptor_read(handle, bytes + total, length - total);
        if (result == kDescriptorWouldBlock) {
            if (total == 0 && !wait_first) {
                return false;
            }
            wait.revents = 0;
            if (descriptor_wait(&wait, 1) < 0) {
                yield();
            }
            continue;
        }
        if (result <= 0) {
            return false;
        }
        total += static_cast<size_t>(result);
    }
    return true;
}

inline void init_request(Request& request, uint16_t command) {
    memset(&request, 0, sizeof(request));
    request.magic = kMessageMagic;
    request.version = kMessageVersion;
    request.command = command;
}

inline uint64_t request_authorization(const Request& request) {
    // Bind the kernel-authenticated sender event to the full request. FNV-1a
    // is sufficient here: this is an integrity correlation value, while the
    // sender PID and ProcessControl authority come from the kernel event.
    const auto* bytes = reinterpret_cast<const uint8_t*>(&request);
    uint64_t hash = 1469598103934665603ull;
    for (size_t i = 0; i < sizeof(request); ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

}  // namespace service_protocol
