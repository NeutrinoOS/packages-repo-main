#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../crt/syscall.hpp"

namespace service {

constexpr const char kNetworkService[] = "net.neutrino.network";
constexpr const char kTcpService[] = "net.neutrino.tcp";
constexpr const char kDhcpService[] = "net.neutrino.dhcp";
constexpr uint32_t kAbiV1 = 1;
constexpr size_t kDefaultLookupSpins = 120000;

inline void copy_cstr(char* dest, size_t capacity, const char* src) {
    if (dest == nullptr || capacity == 0) {
        return;
    }
    size_t i = 0;
    if (src != nullptr) {
        while (src[i] != '\0' && i + 1 < capacity) {
            dest[i] = src[i];
            ++i;
        }
    }
    dest[i] = '\0';
}

inline bool advertise(uint32_t registrar,
                      const char* service,
                      uint32_t abi_version,
                      uint32_t pipe_id,
                      const char* provider) {
    descriptor_defs::ServiceOffer offer{};
    copy_cstr(offer.service, sizeof(offer.service), service);
    copy_cstr(offer.provider, sizeof(offer.provider), provider);
    offer.abi_version = abi_version;
    offer.pipe_id = pipe_id;
    return service_register_offer(registrar, &offer) == 0;
}

inline bool lookup(const char* service,
                   uint32_t abi_version,
                   descriptor_defs::ServiceBinding& binding,
                   size_t spins = kDefaultLookupSpins) {
    for (size_t i = 0; i < spins; ++i) {
        long handle = service_lookup_open(service, abi_version);
        if (handle >= 0) {
            long result = service_get_binding(static_cast<uint32_t>(handle),
                                              &binding);
            descriptor_close(static_cast<uint32_t>(handle));
            if (result == 0 && binding.pipe_id != 0) {
                return true;
            }
        }
        yield();
    }
    return false;
}

inline bool lookup_pipe(const char* service,
                        uint32_t abi_version,
                        uint32_t& pipe_id,
                        size_t spins = kDefaultLookupSpins) {
    descriptor_defs::ServiceBinding binding{};
    if (!lookup(service, abi_version, binding, spins)) {
        return false;
    }
    pipe_id = binding.pipe_id;
    return true;
}

inline long open_provider_pipe(const char* service,
                               uint32_t abi_version,
                               uint64_t flags,
                               size_t spins = kDefaultLookupSpins) {
    uint32_t pipe_id = 0;
    if (!lookup_pipe(service, abi_version, pipe_id, spins)) {
        return -1;
    }
    return pipe_open_existing(flags, pipe_id);
}

}  // namespace service
