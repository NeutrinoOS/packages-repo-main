#include <neutrino/http.hpp>

#include <string.h>

namespace http {

bool headers_complete(const char* data, size_t length) {
    if (data == nullptr || length < 4) {
        return false;
    }
    for (size_t i = 0; i + 3 < length; ++i) {
        if (data[i] == '\r' && data[i + 1] == '\n' &&
            data[i + 2] == '\r' && data[i + 3] == '\n') {
            return true;
        }
    }
    return false;
}

bool parse_request_buffer(char* data, size_t length, Request& request) {
    if (data == nullptr || length == 0) {
        return false;
    }
    size_t line_end = length;
    for (size_t i = 0; i + 1 < length; ++i) {
        if (data[i] == '\r' && data[i + 1] == '\n') {
            line_end = i;
            break;
        }
    }
    if (line_end == length) {
        return false;
    }
    char saved = data[line_end];
    data[line_end] = '\0';
    bool ok = parse_request_line(data, request);
    data[line_end] = saved;
    if (!ok) {
        return false;
    }
    request.header_count = 0;
    size_t cursor = line_end + 2;
    while (cursor + 1 < length) {
        if (data[cursor] == '\r' && data[cursor + 1] == '\n') {
            return true;
        }
        size_t header_end = cursor;
        while (header_end + 1 < length &&
               !(data[header_end] == '\r' && data[header_end + 1] == '\n')) {
            ++header_end;
        }
        if (header_end + 1 >= length) {
            return false;
        }
        saved = data[header_end];
        data[header_end] = '\0';
        Header header{};
        ok = parse_header_line(data + cursor, header);
        data[header_end] = saved;
        if (ok && request.header_count < kMaxHeaders) {
            request.headers[request.header_count++] = header;
        }
        cursor = header_end + 2;
    }
    return false;
}

bool write_error(net::Connection& connection,
                 const char* status,
                 const char* content_type,
                 const char* body) {
    Response response{};
    init_response(response);
    copy_range(response.version, sizeof(response.version), "HTTP/1.0",
               "HTTP/1.0" + 8);
    int code = 0;
    const char* reason = status;
    while (*reason != '\0' && *reason != ' ') {
        if (*reason < '0' || *reason > '9') {
            break;
        }
        code = code * 10 + (*reason - '0');
        ++reason;
    }
    if (*reason == ' ') {
        ++reason;
    }
    response.status_code = code == 0 ? 500 : code;
    copy_range(response.reason, sizeof(response.reason), reason,
               reason + strlen(reason));
    if (content_type != nullptr) {
        add_header(response.headers, kMaxHeaders, response.header_count,
                   "Content-Type", content_type);
    }
    add_header(response.headers, kMaxHeaders, response.header_count,
               "Connection", "close");
    size_t body_length = body == nullptr ? 0 : strlen(body);
    return write_response(connection, response, body, body_length);
}

}  // namespace http
