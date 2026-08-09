#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../crt/syscall.hpp"
#include "service_protocol.hpp"

namespace {

uint32_t g_output = kInvalidDescriptor;

void print(const char* text, size_t length) {
    if (g_output != kInvalidDescriptor && text != nullptr && length != 0) {
        descriptor_write(g_output, text, length);
    }
}

void print(const char* text) {
    if (text != nullptr) {
        print(text, strlen(text));
    }
}

const char* skip_spaces(const char* text) {
    while (text != nullptr && *text == ' ') {
        ++text;
    }
    return text;
}

bool next_token(const char*& cursor, char* out, size_t out_size) {
    cursor = skip_spaces(cursor);
    if (cursor == nullptr || *cursor == '\0' || out == nullptr || out_size == 0) {
        return false;
    }
    size_t length = 0;
    while (*cursor != '\0' && *cursor != ' ') {
        if (length + 1 >= out_size) {
            return false;
        }
        out[length++] = *cursor++;
    }
    out[length] = '\0';
    return length != 0;
}

uint16_t parse_command(const char* command) {
    if (strcmp(command, "list") == 0) return service_protocol::kList;
    if (strcmp(command, "status") == 0) return service_protocol::kStatus;
    if (strcmp(command, "start") == 0) return service_protocol::kStart;
    if (strcmp(command, "stop") == 0) return service_protocol::kStop;
    if (strcmp(command, "restart") == 0) return service_protocol::kRestart;
    if (strcmp(command, "logs") == 0) return service_protocol::kLogs;
    return 0;
}

void usage() {
    print("usage: service list\n"
          "       service status [name]\n"
          "       service start|stop|restart|logs <name>\n");
}

}  // namespace

int main(uint64_t arg, uint64_t) {
    long output = process_get_standard_descriptor(1);
    if (output < 0) {
        output = descriptor_open(
            static_cast<uint32_t>(descriptor_defs::Type::Console), 0);
    }
    if (output >= 0) {
        g_output = static_cast<uint32_t>(output);
    }

    const char* cursor = reinterpret_cast<const char*>(arg);
    char command_text[16];
    if (!next_token(cursor, command_text, sizeof(command_text))) {
        usage();
        return 2;
    }
    uint16_t command = parse_command(command_text);
    if (command == 0) {
        usage();
        return 2;
    }

    service_protocol::Request request{};
    service_protocol::init_request(request, command);
    bool has_service = next_token(cursor, request.service, sizeof(request.service));
    if ((command == service_protocol::kStart ||
         command == service_protocol::kStop ||
         command == service_protocol::kRestart ||
         command == service_protocol::kLogs) && !has_service) {
        usage();
        return 2;
    }

    long registry_handle = shared_memory_open(service_protocol::kRegistryName,
                                               sizeof(service_protocol::Registry));
    if (registry_handle < 0) {
        print("service: init service manager is unavailable\n");
        return 1;
    }
    descriptor_defs::SharedMemoryInfo registry_info{};
    if (shared_memory_get_info(static_cast<uint32_t>(registry_handle),
                               &registry_info) != 0 ||
        registry_info.base == 0 ||
        registry_info.length < sizeof(service_protocol::Registry)) {
        print("service: invalid service manager registry\n");
        return 1;
    }
    const auto* registry = reinterpret_cast<const service_protocol::Registry*>(
        registry_info.base);
    if (registry->magic != service_protocol::kRegistryMagic ||
        registry->version != service_protocol::kRegistryVersion ||
        registry->server_pipe_id == 0) {
        print("service: service manager is not ready\n");
        return 1;
    }

    uint64_t read_flags = static_cast<uint64_t>(descriptor_defs::Flag::Readable) |
                          static_cast<uint64_t>(descriptor_defs::Flag::Writable) |
                          static_cast<uint64_t>(descriptor_defs::Flag::Async);
    long reply = pipe_open_new(read_flags);
    if (reply < 0) {
        print("service: failed to create reply pipe\n");
        return 1;
    }
    descriptor_defs::PipeInfo reply_info{};
    if (pipe_get_info(static_cast<uint32_t>(reply), &reply_info) != 0 ||
        reply_info.id == 0) {
        print("service: failed to inspect reply pipe\n");
        return 1;
    }
    request.reply_pipe_id = reply_info.id;
    request.client_process_id = process_id();

    uint64_t write_flags = static_cast<uint64_t>(descriptor_defs::Flag::Writable);
    long server = pipe_open_existing(write_flags, registry->server_pipe_id);
    if (server < 0) {
        print("service: failed to contact init\n");
        return 1;
    }
    if (process_event_send(registry->manager_process_id,
                           service_protocol::kAuthorizationEvent,
                           service_protocol::request_authorization(request)) != 0) {
        print("service: permission denied\n");
        return 1;
    }
    if (!service_protocol::write_all(static_cast<uint32_t>(server),
                                     &request,
                                     sizeof(request))) {
        print("service: failed to contact init\n");
        return 1;
    }
    descriptor_close(static_cast<uint32_t>(server));

    service_protocol::ResponseHeader response{};
    if (!service_protocol::read_all(static_cast<uint32_t>(reply),
                                    &response,
                                    sizeof(response),
                                    true) ||
        response.magic != service_protocol::kMessageMagic ||
        response.version != service_protocol::kMessageVersion ||
        response.text_length > service_protocol::kResponseTextSize) {
        print("service: invalid response from init\n");
        return 1;
    }

    auto* text = static_cast<char*>(
        map_anonymous(service_protocol::kResponseTextSize, MAP_WRITE));
    if (text == nullptr) {
        print("service: failed to allocate response buffer\n");
        return 1;
    }
    if (response.text_length != 0 &&
        !service_protocol::read_all(static_cast<uint32_t>(reply),
                                    text,
                                    response.text_length,
                                    true)) {
        print("service: incomplete response from init\n");
        return 1;
    }
    print(text, response.text_length);
    return response.status == service_protocol::kStatusOk ? 0 : 1;
}
