#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../crt/syscall.hpp"
#include "../helpers/http.hpp"
#include "../net/dns.hpp"
#include "../net/libnet.hpp"

namespace {

constexpr uint32_t kConnectWaitLimit = 200000;

void print(const char* text) {
    static int32_t console = -1;
    if (console < 0) {
        console = static_cast<int32_t>(
            descriptor_open(static_cast<uint32_t>(descriptor_defs::Type::Console), 0));
        if (console < 0) {
            return;
        }
    }
    if (text == nullptr) {
        return;
    }
    size_t len = strlen(text);
    if (len != 0) {
        descriptor_write(static_cast<uint32_t>(console), text, len);
    }
}

void print_line(const char* text) {
    print(text);
    print("\n");
}

bool append_bytes(char* dest,
                  size_t capacity,
                  size_t& length,
                  const char* begin,
                  const char* end) {
    if (dest == nullptr || begin == nullptr || end == nullptr || begin > end) {
        return false;
    }
    size_t count = static_cast<size_t>(end - begin);
    if (length + count > capacity) {
        return false;
    }
    for (size_t i = 0; i < count; ++i) {
        dest[length + i] = begin[i];
    }
    length += count;
    return true;
}

bool append_string(char* dest, size_t capacity, size_t& length, const char* text) {
    return append_bytes(dest, capacity, length, text, text + strlen(text));
}

bool write_console(const uint8_t* bytes, size_t length) {
    static int32_t console = -1;
    if (console < 0) {
        console = static_cast<int32_t>(
            descriptor_open(static_cast<uint32_t>(descriptor_defs::Type::Console), 0));
        if (console < 0) {
            return false;
        }
    }
    size_t offset = 0;
    while (offset < length) {
        long written =
            descriptor_write(static_cast<uint32_t>(console), bytes + offset, length - offset);
        if (written <= 0) {
            return false;
        }
        offset += static_cast<size_t>(written);
    }
    return true;
}

}  // namespace

int main(uint64_t arg_ptr, uint64_t) {
    const char* args = reinterpret_cast<const char*>(arg_ptr);
    userspace::http::Url target{};
    if (!userspace::http::parse_url(args,
                                    target,
                                    userspace::http::UrlParseMode::DefaultHttp)) {
        print_line("usage: netget <host[:port][/path]>");
        return 1;
    }
    if (target.scheme != userspace::http::Scheme::Http) {
        print_line("netget: https is not supported; use download for TLS");
        return 1;
    }

    uint8_t target_ip[4];
    if (!userspace::http::parse_ipv4_literal(target.host, target_ip)) {
        if (!usernet::dns::resolve_a(target.host, target_ip)) {
            print_line("netget: dns lookup failed");
            return 1;
        }
    }

    net::Connection connection{};
    if (!net::connect_ip(connection, target_ip, target.port)) {
        print_line("netget: connect failed");
        return 1;
    }

    char request[net::kMaxPayload];
    size_t request_length = 0;
    const char request_prefix[] = "GET ";
    const char request_middle[] = " HTTP/1.0\r\nHost: ";
    const char request_suffix[] = "\r\nConnection: close\r\n\r\n";
    if (!append_string(request, sizeof(request), request_length, request_prefix) ||
        !append_string(request, sizeof(request), request_length, target.path) ||
        !append_string(request, sizeof(request), request_length, request_middle) ||
        !append_string(request, sizeof(request), request_length, target.host) ||
        !append_string(request, sizeof(request), request_length, request_suffix)) {
        print_line("netget: request too long");
        return 1;
    }
    if (!net::write(connection, request, request_length)) {
        print_line("netget: failed to send request");
        return 1;
    }

    uint8_t response[1024];
    for (;;) {
        int read = net::read(connection, response, sizeof(response));
        if (read == 0) {
            continue;
        }
        if (read < 0) {
            net::close_connection(connection);
            return 0;
        }
        if (!write_console(response, static_cast<size_t>(read))) {
            net::close_connection(connection);
            return 1;
        }
    }
}
