#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../crt/syscall.hpp"

namespace dhcp_protocol {

constexpr uint32_t kMessageMagic = 0x44484350u;  // "DHCP"
constexpr uint16_t kMessageVersion = 1;

enum MessageType : uint16_t {
    kGetLeaseRequest = 1,
    kGetLeaseResponse = 0x8001,
};

enum Status : int32_t {
    kStatusOk = 0,
    kStatusUnavailable = -1,
};

struct GetLeaseRequest {
    uint32_t reply_pipe_id;
};

struct Lease {
    int32_t status;
    uint8_t address[4];
    uint8_t mask[4];
    uint8_t router[4];
    uint8_t dns[4];
    uint8_t server[4];
    uint32_t reserved;
};

struct Message {
    uint32_t magic;
    uint16_t version;
    uint16_t type;
    union {
        GetLeaseRequest get_lease_request;
        Lease lease;
    };
};

inline void init_message(Message& message, uint16_t type) {
    memset(&message, 0, sizeof(Message));
    message.magic = kMessageMagic;
    message.version = kMessageVersion;
    message.type = type;
}

inline bool write_message(uint32_t handle, const Message& message) {
    const auto* bytes = reinterpret_cast<const uint8_t*>(&message);
    size_t written = 0;
    descriptor_defs::DescriptorWait wait{};
    wait.handle = handle;
    wait.events = descriptor_defs::kWaitWrite;
    while (written < sizeof(Message)) {
        long result = descriptor_write(handle, bytes + written,
                                       sizeof(Message) - written);
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

inline bool read_message(uint32_t handle, Message& message) {
    auto* bytes = reinterpret_cast<uint8_t*>(&message);
    size_t total = 0;
    descriptor_defs::DescriptorWait wait{};
    wait.handle = handle;
    wait.events = descriptor_defs::kWaitRead;
    while (total < sizeof(Message)) {
        long result = descriptor_read(handle, bytes + total,
                                      sizeof(Message) - total);
        if (result == kDescriptorWouldBlock) {
            if (total == 0) {
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
    return message.magic == kMessageMagic &&
           message.version == kMessageVersion;
}

}  // namespace dhcp_protocol
