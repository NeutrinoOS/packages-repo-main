#pragma once

#include <stddef.h>
#include <stdint.h>

namespace net {

constexpr uint32_t kConnectWaitLimit = 200000;
constexpr uint32_t kListenWaitLimit = 200000;
constexpr size_t kMaxPayload = 1460;

struct Connection {
    uint32_t server_pipe = 0;
    uint32_t reply_pipe = 0;
    uint32_t endpoint = 0;
    uint32_t connection_id = 0;
    bool closed = false;
    bool own_server_pipe = true;
    bool own_reply_pipe = true;
    uint8_t pending[kMaxPayload]{};
    size_t pending_offset = 0;
    size_t pending_length = 0;
};

struct Listener {
    uint32_t server_pipe = 0;
    uint32_t reply_pipe = 0;
    uint16_t port = 0;
    bool bound = false;
};

struct Lease {
    int32_t status;
    uint8_t address[4];
    uint8_t mask[4];
    uint8_t router[4];
    uint8_t dns[4];
    uint8_t server[4];
};

void close_connection(Connection& conn);
bool connect_ip(Connection& conn, const uint8_t ip[4], uint16_t port);
bool resolve(const char* host, uint8_t ip[4]);
bool connect(Connection& conn, const char* host, uint16_t port);
bool listen(Listener& listener, uint16_t port);
void close_listener(Listener& listener);
bool accept(Listener& listener, Connection& conn);
bool write(Connection& conn, const void* data, size_t length);
int read(Connection& conn, void* data, size_t capacity);
bool dhcp_lease(Lease& lease);

}  // namespace net
