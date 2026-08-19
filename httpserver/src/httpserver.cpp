#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../crt/syscall.hpp"
#include "../helpers/args.hpp"
#include "../helpers/http.hpp"
#include "../net/libnet.hpp"

namespace {

constexpr uint16_t kDefaultPort = 8000;
constexpr size_t kMaxConnections = 16;
constexpr size_t kMaxRequestBytes = 4096;
constexpr size_t kIoBufferSize = 1200;
constexpr size_t kMaxPathBytes = 1024;

struct Connection {
    bool in_use;
    uint32_t id;
    uint32_t endpoint;
    size_t request_length;
    char request[kMaxRequestBytes + 1];
};

void print(const char* text) {
    static int32_t console = -1;
    if (console < 0) {
        console = static_cast<int32_t>(
            descriptor_open(static_cast<uint32_t>(descriptor_defs::Type::Console), 0));
    }
    if (console < 0 || text == nullptr) {
        return;
    }
    size_t length = strlen(text);
    if (length != 0) {
        (void)descriptor_write(static_cast<uint32_t>(console), text, length);
    }
}

void print_u32(uint32_t value) {
    char digits[11];
    size_t length = 0;
    do {
        digits[length++] = static_cast<char>('0' + value % 10u);
        value /= 10u;
    } while (value != 0);
    char output[11];
    size_t output_length = 0;
    while (length != 0) {
        output[output_length++] = digits[--length];
    }
    output[output_length] = '\0';
    print(output);
}

bool parse_port(const char* text, uint16_t& port) {
    if (text == nullptr || *text == '\0') {
        return false;
    }
    uint32_t value = 0;
    while (*text != '\0') {
        if (*text < '0' || *text > '9') {
            return false;
        }
        value = value * 10u + static_cast<uint32_t>(*text++ - '0');
        if (value > 65535u) {
            return false;
        }
    }
    if (value == 0) {
        return false;
    }
    port = static_cast<uint16_t>(value);
    return true;
}

bool parse_arguments(const char* args, uint16_t& port) {
    port = kDefaultPort;
    const char* cursor = args;
    char option[32];
    if (!userspace::copy_token(cursor, option, sizeof(option))) {
        return userspace::only_spaces_remain(cursor);
    }
    if (strcmp(option, "--port") != 0 && strcmp(option, "-p") != 0) {
        return false;
    }
    char value[16];
    return userspace::copy_token(cursor, value, sizeof(value)) &&
           userspace::only_spaces_remain(cursor) &&
           parse_port(value, port);
}

Connection* allocate_connection(Connection* connections) {
    for (size_t i = 0; i < kMaxConnections; ++i) {
        if (!connections[i].in_use) {
            return &connections[i];
        }
    }
    return nullptr;
}

void release_connection(Connection& connection) {
    if (connection.endpoint != 0) {
        descriptor_close(connection.endpoint);
    }
    connection.in_use = false;
    connection.id = 0;
    connection.endpoint = 0;
    connection.request_length = 0;
    connection.request[0] = '\0';
}

bool endpoint_write_all(uint32_t endpoint, const void* data, size_t length) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    size_t written = 0;
    descriptor_defs::DescriptorWait wait{};
    wait.handle = endpoint;
    wait.events = descriptor_defs::kWaitWrite;
    while (written < length) {
        long result = descriptor_write(endpoint, bytes + written, length - written);
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

bool append_text(char* output, size_t capacity, size_t& length, const char* text) {
    while (text != nullptr && *text != '\0') {
        if (length + 1 >= capacity) {
            return false;
        }
        output[length++] = *text++;
    }
    output[length] = '\0';
    return true;
}

bool append_u64(char* output, size_t capacity, size_t& length, uint64_t value) {
    char digits[21];
    size_t count = 0;
    do {
        digits[count++] = static_cast<char>('0' + value % 10u);
        value /= 10u;
    } while (value != 0);
    while (count != 0) {
        if (length + 1 >= capacity) {
            return false;
        }
        output[length++] = digits[--count];
    }
    output[length] = '\0';
    return true;
}

bool send_header(uint32_t endpoint,
                 const char* status,
                 const char* content_type,
                 uint64_t content_length) {
    char header[512];
    size_t length = 0;
    return append_text(header, sizeof(header), length, "HTTP/1.0 ") &&
           append_text(header, sizeof(header), length, status) &&
           append_text(header, sizeof(header), length, "\r\nServer: neutrino-httpserver/1.0\r\n") &&
           append_text(header, sizeof(header), length, "Connection: close\r\nContent-Type: ") &&
           append_text(header, sizeof(header), length, content_type) &&
           append_text(header, sizeof(header), length, "\r\nContent-Length: ") &&
           append_u64(header, sizeof(header), length, content_length) &&
           append_text(header, sizeof(header), length, "\r\n\r\n") &&
           endpoint_write_all(endpoint, header, length);
}

void send_error(uint32_t endpoint, const char* status, const char* body, bool head_only) {
    size_t length = strlen(body);
    if (send_header(endpoint, status, "text/plain; charset=utf-8", length) && !head_only) {
        (void)endpoint_write_all(endpoint, body, length);
    }
}

bool safe_path_from_target(const char* target, size_t target_length, char* output) {
    if (target_length == 0 || target[0] != '/') {
        return false;
    }
    char decoded[kMaxPathBytes];
    size_t decoded_length = userspace::http::percent_decode(
        target + 1, target_length - 1, decoded, sizeof(decoded));
    size_t out_length = 0;
    for (size_t i = 0; i < decoded_length && decoded[i] != '?' && decoded[i] != '#'; ++i) {
        unsigned char ch = static_cast<unsigned char>(decoded[i]);
        if (ch == 0 || ch == '\\' || ch < 0x20 || ch == 0x7f ||
            out_length + 1 >= kMaxPathBytes) {
            return false;
        }
        output[out_length++] = static_cast<char>(ch);
    }
    if (out_length == 0) {
        const char index_name[] = "index.html";
        for (size_t i = 0; i < sizeof(index_name); ++i) {
            output[i] = index_name[i];
        }
        return true;
    }
    output[out_length] = '\0';

    const char* segment = output;
    for (size_t i = 0; i <= out_length; ++i) {
        if (output[i] != '/' && output[i] != '\0') {
            continue;
        }
        size_t segment_length = static_cast<size_t>(output + i - segment);
        if (segment_length == 0 ||
            (segment_length == 1 && segment[0] == '.') ||
            (segment_length == 2 && segment[0] == '.' && segment[1] == '.')) {
            return false;
        }
        segment = output + i + 1;
    }
    return true;
}

const char* content_type_for(const char* path) {
    const char* extension = nullptr;
    for (const char* cursor = path; *cursor != '\0'; ++cursor) {
        if (*cursor == '.') extension = cursor;
        if (*cursor == '/') extension = nullptr;
    }
    if (extension == nullptr) return "application/octet-stream";
    if (strcmp(extension, ".html") == 0 || strcmp(extension, ".htm") == 0)
        return "text/html; charset=utf-8";
    if (strcmp(extension, ".css") == 0) return "text/css; charset=utf-8";
    if (strcmp(extension, ".js") == 0) return "text/javascript; charset=utf-8";
    if (strcmp(extension, ".txt") == 0) return "text/plain; charset=utf-8";
    if (strcmp(extension, ".json") == 0) return "application/json";
    if (strcmp(extension, ".png") == 0) return "image/png";
    if (strcmp(extension, ".jpg") == 0 || strcmp(extension, ".jpeg") == 0) return "image/jpeg";
    if (strcmp(extension, ".gif") == 0) return "image/gif";
    if (strcmp(extension, ".svg") == 0) return "image/svg+xml";
    if (strcmp(extension, ".ico") == 0) return "image/x-icon";
    if (strcmp(extension, ".pdf") == 0) return "application/pdf";
    return "application/octet-stream";
}

long file_stat(uint32_t handle, FileMetadata* metadata) {
    return raw_syscall2(SystemCall::FileStat,
                        static_cast<long>(handle),
                        reinterpret_cast<long>(metadata));
}

void serve_request(Connection& connection) {
    char* line_end = nullptr;
    for (size_t i = 0; i + 1 < connection.request_length; ++i) {
        if (connection.request[i] == '\r' && connection.request[i + 1] == '\n') {
            line_end = connection.request + i;
            break;
        }
    }
    if (line_end == nullptr) {
        send_error(connection.endpoint, "400 Bad Request", "Bad Request\n", false);
        return;
    }
    *line_end = '\0';
    userspace::http::Request parsed{};
    if (!userspace::http::parse_request_line(connection.request, parsed)) {
        send_error(connection.endpoint, "400 Bad Request", "Bad Request\n", false);
        return;
    }
    bool head_only = strcmp(parsed.method, "HEAD") == 0;
    if (strcmp(parsed.method, "GET") != 0 && !head_only) {
        send_error(connection.endpoint, "405 Method Not Allowed", "Method Not Allowed\n", false);
        return;
    }
    size_t target_length = strlen(parsed.target);
    char path[kMaxPathBytes];
    if (!safe_path_from_target(parsed.target, target_length, path)) {
        send_error(connection.endpoint, "403 Forbidden", "Forbidden\n", head_only);
        return;
    }

    long file = file_open(path);
    if (file < 0) {
        send_error(connection.endpoint, "404 Not Found", "Not Found\n", head_only);
        return;
    }
    FileMetadata metadata{};
    if (file_stat(static_cast<uint32_t>(file), &metadata) != 0 ||
        (metadata.flags & FILE_METADATA_DIRECTORY) != 0) {
        file_close(static_cast<uint32_t>(file));
        send_error(connection.endpoint, "404 Not Found", "Not Found\n", head_only);
        return;
    }
    if (!send_header(connection.endpoint, "200 OK", content_type_for(path), metadata.size) ||
        head_only) {
        file_close(static_cast<uint32_t>(file));
        return;
    }
    uint8_t buffer[kIoBufferSize];
    for (;;) {
        long count = file_read(static_cast<uint32_t>(file), buffer, sizeof(buffer));
        if (count <= 0 ||
            !endpoint_write_all(connection.endpoint, buffer, static_cast<size_t>(count))) {
            break;
        }
    }
    file_close(static_cast<uint32_t>(file));
}

bool request_complete(const Connection& connection) {
    for (size_t i = 0; i + 3 < connection.request_length; ++i) {
        if (connection.request[i] == '\r' && connection.request[i + 1] == '\n' &&
            connection.request[i + 2] == '\r' && connection.request[i + 3] == '\n') {
            return true;
        }
    }
    return false;
}

void poll_connections(Connection* connections) {
    for (size_t i = 0; i < kMaxConnections; ++i) {
        Connection& connection = connections[i];
        if (!connection.in_use) {
            continue;
        }
        bool close = false;
        while (!close) {
            size_t available = kMaxRequestBytes - connection.request_length;
            if (available == 0) {
                send_error(connection.endpoint,
                           "431 Request Header Fields Too Large",
                           "Request Header Too Large\n",
                           false);
                close = true;
                break;
            }
            long count = descriptor_read(connection.endpoint,
                                         connection.request + connection.request_length,
                                         available);
            if (count == kDescriptorWouldBlock) {
                break;
            }
            if (count <= 0) {
                close = true;
                break;
            }
            connection.request_length += static_cast<size_t>(count);
            connection.request[connection.request_length] = '\0';
            if (request_complete(connection)) {
                serve_request(connection);
                close = true;
            }
        }
        if (close) {
            release_connection(connection);
        }
    }
}

}  // namespace

int main(uint64_t arg_ptr, uint64_t) {
    uint16_t port = kDefaultPort;
    if (!parse_arguments(reinterpret_cast<const char*>(arg_ptr), port)) {
        print("usage: httpserver [--port PORT]\n");
        return 1;
    }

    net::Listener listener{};
    if (!net::listen(listener, port)) {
        print("httpserver: TCP service is unavailable\n");
        return 1;
    }
    print("httpserver: serving the current directory on port ");
    print_u32(port);
    print("\n");

    Connection connections[kMaxConnections]{};
    descriptor_defs::DescriptorWait waits[kMaxConnections + 1]{};
    for (;;) {
        poll_connections(connections);
        net::Connection accepted{};
        if (!net::accept(listener, accepted)) {
            size_t wait_count = 0;
            waits[wait_count].handle = listener.reply_pipe;
            waits[wait_count].events = descriptor_defs::kWaitRead;
            waits[wait_count].revents = 0;
            waits[wait_count].reserved = 0;
            ++wait_count;

            for (size_t i = 0; i < kMaxConnections; ++i) {
                if (!connections[i].in_use || connections[i].endpoint == 0) {
                    continue;
                }
                waits[wait_count].handle = connections[i].endpoint;
                waits[wait_count].events = descriptor_defs::kWaitRead;
                waits[wait_count].revents = 0;
                waits[wait_count].reserved = 0;
                ++wait_count;
            }
            if (descriptor_wait(waits, wait_count) < 0) {
                yield();
            }
            continue;
        }
        Connection* connection = allocate_connection(connections);
        if (connection == nullptr) {
            net::close_connection(accepted);
            continue;
        }
        connection->in_use = true;
        connection->id = accepted.connection_id;
        connection->endpoint = accepted.endpoint;
        accepted.endpoint = 0;
        connection->request_length = 0;
        connection->request[0] = '\0';
    }
}
