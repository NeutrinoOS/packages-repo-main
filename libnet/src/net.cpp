#include <neutrino/http.hpp>
#include <neutrino/net.hpp>

#include <string.h>

#include "crt/syscall.hpp"
#include "net/dhcp_protocol.hpp"
#include "net/dns.hpp"
#include "net/network_protocol.hpp"
#include "net/service.hpp"
#include "net/tcpd_protocol.hpp"

static_assert(net::kMaxPayload == tcpd_protocol::kMaxPayload,
              "libnet payload size must match the TCP ABI");

namespace net {
namespace {

uint64_t async_read_flags() {
    return static_cast<uint64_t>(descriptor_defs::Flag::Readable) |
           static_cast<uint64_t>(descriptor_defs::Flag::Async);
}

uint64_t async_write_flags() {
    return static_cast<uint64_t>(descriptor_defs::Flag::Writable) |
           static_cast<uint64_t>(descriptor_defs::Flag::Async);
}

bool pipe_id_of(uint32_t handle, uint32_t& id) {
    descriptor_defs::PipeInfo info{};
    if (pipe_get_info(handle, &info) != 0 || info.id == 0) {
        return false;
    }
    id = info.id;
    return true;
}

long open_tcp_provider() {
    return service::open_provider_pipe(service::kTcpService, service::kAbiV1,
                                       async_write_flags());
}

}  // namespace

void close_connection(Connection& conn) {
    if (conn.server_pipe != 0 && conn.connection_id != 0) {
        tcpd_protocol::Message message{};
        tcpd_protocol::init_message(message, tcpd_protocol::kCloseRequest);
        message.close_request.connection_id = conn.connection_id;
        (void)tcpd_protocol::write_message(conn.server_pipe, message);
    }
    if (conn.endpoint != 0) {
        descriptor_close(conn.endpoint);
    }
    if (conn.own_reply_pipe && conn.reply_pipe != 0) {
        descriptor_close(conn.reply_pipe);
    }
    if (conn.own_server_pipe && conn.server_pipe != 0) {
        descriptor_close(conn.server_pipe);
    }
    conn = Connection{};
    conn.closed = true;
}

bool connect_ip(Connection& conn, const uint8_t ip[4], uint16_t port) {
    close_connection(conn);
    conn.closed = false;
    long server = open_tcp_provider();
    if (server < 0) {
        return false;
    }
    long reply = pipe_open_new(async_read_flags());
    uint32_t reply_id = 0;
    if (reply < 0 || !pipe_id_of(static_cast<uint32_t>(reply), reply_id)) {
        if (reply >= 0) descriptor_close(static_cast<uint32_t>(reply));
        descriptor_close(static_cast<uint32_t>(server));
        return false;
    }

    tcpd_protocol::Message request{};
    tcpd_protocol::init_message(request, tcpd_protocol::kConnectRequest);
    request.connect_request.reply_pipe_id = reply_id;
    request.connect_request.remote_port = port;
    for (size_t i = 0; i < 4; ++i) {
        request.connect_request.remote_ip[i] = ip[i];
    }
    if (!tcpd_protocol::write_message(static_cast<uint32_t>(server), request)) {
        descriptor_close(static_cast<uint32_t>(reply));
        descriptor_close(static_cast<uint32_t>(server));
        return false;
    }

    uint32_t waits = 0;
    for (;;) {
        tcpd_protocol::Message message{};
        if (!tcpd_protocol::read_message(static_cast<uint32_t>(reply), message)) {
            if (waits++ >= kConnectWaitLimit) {
                descriptor_close(static_cast<uint32_t>(reply));
                descriptor_close(static_cast<uint32_t>(server));
                return false;
            }
            yield();
            continue;
        }
        if (message.type != tcpd_protocol::kConnectResponse) {
            continue;
        }
        if (message.connect_response.status != tcpd_protocol::kStatusOk) {
            descriptor_close(static_cast<uint32_t>(reply));
            descriptor_close(static_cast<uint32_t>(server));
            return false;
        }
        conn.server_pipe = static_cast<uint32_t>(server);
        conn.reply_pipe = static_cast<uint32_t>(reply);
        conn.connection_id = message.connect_response.connection_id;
        conn.endpoint = 0;
        if (message.connect_response.endpoint_id != 0) {
            uint64_t endpoint_flags =
                static_cast<uint64_t>(descriptor_defs::Flag::Readable) |
                static_cast<uint64_t>(descriptor_defs::Flag::Writable) |
                static_cast<uint64_t>(descriptor_defs::Flag::Async);
            long endpoint = net_endpoint_open_existing(
                endpoint_flags, message.connect_response.endpoint_id);
            if (endpoint >= 0) {
                conn.endpoint = static_cast<uint32_t>(endpoint);
            }
        }
        conn.closed = false;
        return true;
    }
}

bool resolve(const char* host, uint8_t ip[4]) {
    if (http::parse_ipv4_literal(host, ip)) {
        return true;
    }
    return usernet::dns::resolve_a(host, ip);
}

bool connect(Connection& conn, const char* host, uint16_t port) {
    uint8_t ip[4]{};
    if (!resolve(host, ip)) {
        return false;
    }
    return connect_ip(conn, ip, port);
}

bool listen(Listener& listener, uint16_t port) {
    listener = Listener{};
    long server = open_tcp_provider();
    if (server < 0) {
        return false;
    }
    long reply = pipe_open_new(async_read_flags());
    uint32_t reply_id = 0;
    if (reply < 0 || !pipe_id_of(static_cast<uint32_t>(reply), reply_id)) {
        if (reply >= 0) descriptor_close(static_cast<uint32_t>(reply));
        descriptor_close(static_cast<uint32_t>(server));
        return false;
    }
    tcpd_protocol::Message request{};
    tcpd_protocol::init_message(request, tcpd_protocol::kListenRequest);
    request.listen_request.reply_pipe_id = reply_id;
    request.listen_request.port = port;
    if (!tcpd_protocol::write_message(static_cast<uint32_t>(server), request)) {
        descriptor_close(static_cast<uint32_t>(reply));
        descriptor_close(static_cast<uint32_t>(server));
        return false;
    }
    uint32_t waits = 0;
    for (;;) {
        tcpd_protocol::Message message{};
        if (!tcpd_protocol::read_message(static_cast<uint32_t>(reply), message)) {
            if (waits++ >= kListenWaitLimit) {
                descriptor_close(static_cast<uint32_t>(reply));
                descriptor_close(static_cast<uint32_t>(server));
                return false;
            }
            yield();
            continue;
        }
        if (message.type != tcpd_protocol::kListenResponse) {
            continue;
        }
        if (message.listen_response.status != tcpd_protocol::kStatusOk) {
            descriptor_close(static_cast<uint32_t>(reply));
            descriptor_close(static_cast<uint32_t>(server));
            return false;
        }
        listener.server_pipe = static_cast<uint32_t>(server);
        listener.reply_pipe = static_cast<uint32_t>(reply);
        listener.port = message.listen_response.port;
        listener.bound = true;
        return true;
    }
}

void close_listener(Listener& listener) {
    if (listener.reply_pipe != 0) descriptor_close(listener.reply_pipe);
    if (listener.server_pipe != 0) descriptor_close(listener.server_pipe);
    listener = Listener{};
}

bool accept(Listener& listener, Connection& conn) {
    if (!listener.bound) {
        return false;
    }
    tcpd_protocol::Message event{};
    if (!tcpd_protocol::read_message(listener.reply_pipe, event)) {
        return false;
    }
    if (event.type != tcpd_protocol::kAcceptEvent ||
        event.accept_event.endpoint_id == 0) {
        return false;
    }
    uint64_t endpoint_flags =
        static_cast<uint64_t>(descriptor_defs::Flag::Readable) |
        static_cast<uint64_t>(descriptor_defs::Flag::Writable) |
        static_cast<uint64_t>(descriptor_defs::Flag::Async);
    long endpoint = net_endpoint_open_existing(endpoint_flags,
                                               event.accept_event.endpoint_id);
    if (endpoint < 0) {
        return false;
    }
    conn = Connection{};
    conn.server_pipe = listener.server_pipe;
    conn.endpoint = static_cast<uint32_t>(endpoint);
    conn.connection_id = event.accept_event.connection_id;
    conn.own_server_pipe = false;
    conn.own_reply_pipe = false;
    return true;
}

bool write(Connection& conn, const void* data, size_t length) {
    if (conn.closed || data == nullptr) {
        return false;
    }
    const auto* bytes = static_cast<const uint8_t*>(data);
    if (conn.endpoint != 0) {
        size_t offset = 0;
        descriptor_defs::DescriptorWait wait{};
        wait.handle = conn.endpoint;
        wait.events = descriptor_defs::kWaitWrite;
        while (offset < length) {
            long result = descriptor_write(conn.endpoint, bytes + offset,
                                           length - offset);
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
            offset += static_cast<size_t>(result);
        }
        return true;
    }
    size_t offset = 0;
    while (offset < length) {
        size_t chunk = length - offset;
        if (chunk > kMaxPayload) {
            chunk = kMaxPayload;
        }
        tcpd_protocol::Message message{};
        tcpd_protocol::init_message(message, tcpd_protocol::kSendRequest);
        message.send_request.connection_id = conn.connection_id;
        message.send_request.payload_length = static_cast<uint16_t>(chunk);
        memcpy(message.send_request.payload, bytes + offset, chunk);
        if (!tcpd_protocol::write_message(conn.server_pipe, message)) {
            return false;
        }
        offset += chunk;
    }
    return true;
}

int read(Connection& conn, void* data, size_t capacity) {
    if (capacity == 0 || data == nullptr) {
        return 0;
    }
    auto* out = static_cast<uint8_t*>(data);
    if (conn.endpoint != 0) {
        descriptor_defs::DescriptorWait wait{};
        wait.handle = conn.endpoint;
        wait.events = descriptor_defs::kWaitRead;
        for (;;) {
            long result = descriptor_read(conn.endpoint, out, capacity);
            if (result == kDescriptorWouldBlock) {
                wait.revents = 0;
                if (descriptor_wait(&wait, 1) < 0) {
                    yield();
                }
                continue;
            }
            if (result <= 0) {
                conn.closed = true;
                return -1;
            }
            return static_cast<int>(result);
        }
    }
    while (conn.pending_offset == conn.pending_length) {
        if (conn.closed) {
            return -1;
        }
        tcpd_protocol::Message message{};
        if (!tcpd_protocol::read_message(conn.reply_pipe, message)) {
            yield();
            continue;
        }
        if (message.type == tcpd_protocol::kDataEvent &&
            message.data_event.connection_id == conn.connection_id) {
            conn.pending_offset = 0;
            conn.pending_length = message.data_event.payload_length;
            if (conn.pending_length > kMaxPayload) {
                conn.pending_length = kMaxPayload;
            }
            memcpy(conn.pending, message.data_event.payload, conn.pending_length);
            break;
        }
        if (message.type == tcpd_protocol::kClosedEvent &&
            message.closed_event.connection_id == conn.connection_id) {
            conn.closed = true;
            return -1;
        }
    }
    size_t available = conn.pending_length - conn.pending_offset;
    if (available > capacity) {
        available = capacity;
    }
    memcpy(out, conn.pending + conn.pending_offset, available);
    conn.pending_offset += available;
    return static_cast<int>(available);
}

bool dhcp_lease(Lease& lease) {
    long server = service::open_provider_pipe(service::kDhcpService,
                                              service::kAbiV1,
                                              async_write_flags());
    if (server < 0) {
        return false;
    }
    long reply = pipe_open_new(async_read_flags());
    if (reply < 0) {
        descriptor_close(static_cast<uint32_t>(server));
        return false;
    }
    uint32_t reply_id = 0;
    if (!pipe_id_of(static_cast<uint32_t>(reply), reply_id)) {
        descriptor_close(static_cast<uint32_t>(reply));
        descriptor_close(static_cast<uint32_t>(server));
        return false;
    }
    dhcp_protocol::Message request{};
    dhcp_protocol::init_message(request, dhcp_protocol::kGetLeaseRequest);
    request.get_lease_request.reply_pipe_id = reply_id;
    bool ok = dhcp_protocol::write_message(static_cast<uint32_t>(server), request);
    dhcp_protocol::Message response{};
    if (ok) {
        ok = false;
        for (size_t i = 0; i < 20000; ++i) {
            if (dhcp_protocol::read_message(static_cast<uint32_t>(reply), response) &&
                response.type == dhcp_protocol::kGetLeaseResponse) {
                ok = response.lease.status == dhcp_protocol::kStatusOk;
                break;
            }
            yield();
        }
    }
    if (ok) {
        lease.status = response.lease.status;
        for (size_t i = 0; i < 4; ++i) {
            lease.address[i] = response.lease.address[i];
            lease.mask[i] = response.lease.mask[i];
            lease.router[i] = response.lease.router[i];
            lease.dns[i] = response.lease.dns[i];
            lease.server[i] = response.lease.server[i];
        }
    }
    descriptor_close(static_cast<uint32_t>(reply));
    descriptor_close(static_cast<uint32_t>(server));
    return ok;
}

}  // namespace net
