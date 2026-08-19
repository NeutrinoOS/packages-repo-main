#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../crt/syscall.hpp"
#include "../net/libnet.hpp"
#include "../net/service.hpp"

namespace {

void print(const char* text) {
    static int32_t console = -1;
    if (console < 0) {
        console = static_cast<int32_t>(
            descriptor_open(static_cast<uint32_t>(descriptor_defs::Type::Console), 0));
    }
    if (console < 0 || text == nullptr) {
        return;
    }
    descriptor_write(static_cast<uint32_t>(console), text, strlen(text));
}

}  // namespace

int main(uint64_t, uint64_t) {
    descriptor_defs::ServiceBinding binding{};
    if (!service::lookup(service::kTcpService, service::kAbiV1, binding, 20000)) {
        print("svctest: no TCP ABI provider\n");
        return 1;
    }
    print("svctest: provider=");
    print(binding.provider);
    print(" pipe=");
    char id[16];
    size_t n = 0;
    uint32_t value = binding.pipe_id;
    char rev[16];
    size_t count = 0;
    do {
        rev[count++] = static_cast<char>('0' + (value % 10));
        value /= 10;
    } while (value != 0);
    while (count != 0) {
        id[n++] = rev[--count];
    }
    id[n] = '\0';
    print(id);
    print("\n");

    net::Connection connection{};
    uint8_t ip[4] = {127, 0, 0, 1};
    if (!net::connect_ip(connection, ip, 80)) {
        print("svctest: connect failed\n");
        return 1;
    }
    const char request[] = "GET / HTTP/1.0\r\nHost: mock\r\n\r\n";
    if (!net::write(connection, request, sizeof(request) - 1)) {
        print("svctest: write failed\n");
        net::close_connection(connection);
        return 1;
    }
    uint8_t buffer[256];
    int got = net::read(connection, buffer, sizeof(buffer) - 1);
    net::close_connection(connection);
    if (got <= 0) {
        print("svctest: connected through TCP ABI (no canned body)\n");
        return 0;
    }
    buffer[got] = 0;
    print(reinterpret_cast<const char*>(buffer));
    print("\n");
    return 0;
}
