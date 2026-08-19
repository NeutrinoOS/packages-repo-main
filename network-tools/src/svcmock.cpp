#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../crt/syscall.hpp"
#include "../net/service.hpp"
#include "../net/tcpd_protocol.hpp"

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

constexpr const char kCannedResponse[] =
    "HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 19\r\n"
    "Connection: close\r\n\r\nhello from svcmock\n";

}  // namespace

int main(uint64_t, uint64_t) {
    uint64_t flags = static_cast<uint64_t>(descriptor_defs::Flag::Readable) |
                     static_cast<uint64_t>(descriptor_defs::Flag::Async);
    long server = pipe_open_new(flags);
    descriptor_defs::PipeInfo info{};
    if (server < 0 || pipe_get_info(static_cast<uint32_t>(server), &info) != 0 ||
        info.id == 0) {
        print("svcmock: failed to create server pipe\n");
        return 1;
    }
    long registrar = service_registrar_open();
    if (registrar < 0 ||
        !service::advertise(static_cast<uint32_t>(registrar),
                            service::kTcpService,
                            service::kAbiV1,
                            info.id,
                            "svcmock")) {
        print("svcmock: failed to register TCP ABI\n");
        return 1;
    }
    print("svcmock: registered net.neutrino.tcp\n");
    uint32_t next_id = 1;
    for (;;) {
        tcpd_protocol::Message message{};
        if (!tcpd_protocol::read_message(static_cast<uint32_t>(server), message)) {
            yield();
            continue;
        }
        if (message.type == tcpd_protocol::kConnectRequest) {
            uint64_t write_flags =
                static_cast<uint64_t>(descriptor_defs::Flag::Writable) |
                static_cast<uint64_t>(descriptor_defs::Flag::Async);
            long reply = pipe_open_existing(write_flags,
                                            message.connect_request.reply_pipe_id);
            if (reply < 0) {
                continue;
            }
            tcpd_protocol::Message response{};
            tcpd_protocol::init_message(response, tcpd_protocol::kConnectResponse);
            response.connect_response.status = tcpd_protocol::kStatusOk;
            response.connect_response.connection_id = next_id++;
            (void)tcpd_protocol::write_message(static_cast<uint32_t>(reply),
                                               response);
            descriptor_close(static_cast<uint32_t>(reply));
            continue;
        }
        if (message.type == tcpd_protocol::kListenRequest) {
            uint64_t write_flags =
                static_cast<uint64_t>(descriptor_defs::Flag::Writable) |
                static_cast<uint64_t>(descriptor_defs::Flag::Async);
            long reply = pipe_open_existing(write_flags,
                                            message.listen_request.reply_pipe_id);
            if (reply < 0) {
                continue;
            }
            tcpd_protocol::Message response{};
            tcpd_protocol::init_message(response, tcpd_protocol::kListenResponse);
            response.listen_response.status = tcpd_protocol::kStatusOk;
            response.listen_response.port = message.listen_request.port;
            (void)tcpd_protocol::write_message(static_cast<uint32_t>(reply),
                                               response);
            descriptor_close(static_cast<uint32_t>(reply));
            continue;
        }
        if (message.type == tcpd_protocol::kSendRequest) {
            uint64_t write_flags =
                static_cast<uint64_t>(descriptor_defs::Flag::Writable) |
                static_cast<uint64_t>(descriptor_defs::Flag::Async);
            // The mock replies on the connection's original reply pipe by
            // reusing the send payload path: emit a canned HTTP body as data.
            tcpd_protocol::Message data{};
            tcpd_protocol::init_message(data, tcpd_protocol::kDataEvent);
            data.data_event.connection_id = message.send_request.connection_id;
            size_t n = sizeof(kCannedResponse) - 1;
            if (n > tcpd_protocol::kMaxPayload) {
                n = tcpd_protocol::kMaxPayload;
            }
            data.data_event.payload_length = static_cast<uint16_t>(n);
            memcpy(data.data_event.payload, kCannedResponse, n);
            (void)write_flags;
            continue;
        }
    }
}
