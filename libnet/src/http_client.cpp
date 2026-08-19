#include <neutrino/http.hpp>

#include <string.h>

namespace http {

bool connection_write(void* context, const void* data, size_t length) {
    auto* io = static_cast<ConnectionIO*>(context);
    return net::write(*io->connection, data, length);
}

bool connection_read(void* context, void* data, size_t length, size_t& got) {
    auto* io = static_cast<ConnectionIO*>(context);
    int n = net::read(*io->connection, data, length);
    if (n < 0) {
        got = 0;
        return false;
    }
    got = static_cast<size_t>(n);
    return n > 0;
}

bool connection_read_line(void* context, char* out, size_t out_size) {
    size_t length = 0;
    while (length + 1 < out_size) {
        uint8_t ch = 0;
        size_t got = 0;
        if (!connection_read(context, &ch, 1, got) || got == 0) {
            return false;
        }
        if (ch == '\r') {
            continue;
        }
        if (ch == '\n') {
            out[length] = '\0';
            return true;
        }
        out[length++] = static_cast<char>(ch);
    }
    out[out_size - 1] = '\0';
    return true;
}

ByteStream connection_stream(ConnectionIO& io) {
    return ByteStream{&io, connection_read, connection_write};
}

bool write_request(net::Connection& connection, const Request& request) {
    ConnectionIO io{&connection};
    ByteStream stream = connection_stream(io);
    return write_request(stream, request);
}

bool write_response(net::Connection& connection, const Response& response,
                    const void* body, size_t body_length) {
    ConnectionIO io{&connection};
    ByteStream stream = connection_stream(io);
    return write_response(stream, response, body, body_length);
}

bool read_response_headers(net::Connection& connection, ResponseMeta& meta) {
    ConnectionIO io{&connection};
    return read_response_headers(&io, connection_read_line, meta);
}

bool request(const char* url_text,
             const char* method,
             const void* body,
             size_t body_length,
             ResponseMeta& meta,
             ByteStream* response_body) {
    Url url{};
    if (!parse_url(url_text, url, UrlParseMode::RequireScheme) ||
        url.scheme != Scheme::Http) {
        return false;
    }
    net::Connection connection{};
    if (!net::connect(connection, url.host, url.port)) {
        return false;
    }
    Request req{};
    init_request(req);
    copy_range(req.method, sizeof(req.method), method, method + strlen(method));
    char target[kMaxPath];
    target[0] = '\0';
    if (!append_cstr(target, sizeof(target), url.path)) {
        net::close_connection(connection);
        return false;
    }
    if (url.query[0] != '\0') {
        if (!append_cstr(target, sizeof(target), "?") ||
            !append_cstr(target, sizeof(target), url.query)) {
            net::close_connection(connection);
            return false;
        }
    }
    copy_range(req.target, sizeof(req.target), target, target + strlen(target));
    if (!add_header(req.headers, kMaxHeaders, req.header_count, "Host", url.host) ||
        !add_header(req.headers, kMaxHeaders, req.header_count, "Connection",
                    "close")) {
        net::close_connection(connection);
        return false;
    }
    req.body = static_cast<const uint8_t*>(body);
    req.body_length = body_length;
    if (!write_request(connection, req) ||
        !read_response_headers(connection, meta)) {
        net::close_connection(connection);
        return false;
    }
    bool ok = true;
    if (response_body != nullptr && response_body->write != nullptr) {
        ConnectionIO io{&connection};
        uint8_t buffer[512];
        for (;;) {
            size_t got = 0;
            if (!connection_read(&io, buffer, sizeof(buffer), got)) {
                break;
            }
            if (got == 0) {
                continue;
            }
            if (!response_body->write(response_body->context, buffer, got)) {
                ok = false;
                break;
            }
        }
    }
    net::close_connection(connection);
    return ok;
}

bool get(const char* url, ResponseMeta& meta, ByteStream* response_body) {
    return request(url, "GET", nullptr, 0, meta, response_body);
}

bool post(const char* url,
          const void* body,
          size_t body_length,
          ResponseMeta& meta,
          ByteStream* response_body) {
    return request(url, "POST", body, body_length, meta, response_body);
}

}  // namespace http
