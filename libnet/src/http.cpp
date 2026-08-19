#include <neutrino/http.hpp>

#include <string.h>

namespace http {

bool is_digit(char ch) {
    return ch >= '0' && ch <= '9';
}

char to_lower(char ch) {
    if (ch >= 'A' && ch <= 'Z') {
        return static_cast<char>(ch - 'A' + 'a');
    }
    return ch;
}

bool starts_with_ci(const char* text, const char* prefix) {
    if (text == nullptr || prefix == nullptr) {
        return false;
    }
    while (*prefix != '\0') {
        if (*text == '\0' || to_lower(*text) != to_lower(*prefix)) {
            return false;
        }
        ++text;
        ++prefix;
    }
    return true;
}

int find_char(const char* text, char ch) {
    if (text == nullptr) {
        return -1;
    }
    for (size_t i = 0; text[i] != '\0'; ++i) {
        if (text[i] == ch) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void copy_range(char* dest, size_t capacity, const char* begin, const char* end) {
    if (dest == nullptr || capacity == 0) {
        return;
    }
    size_t length = 0;
    if (begin != nullptr && end != nullptr && begin <= end) {
        length = static_cast<size_t>(end - begin);
        if (length >= capacity) {
            length = capacity - 1;
        }
        if (length != 0) {
            memcpy(dest, begin, length);
        }
    }
    dest[length] = '\0';
}

bool append_cstr(char* dest, size_t capacity, const char* src) {
    if (dest == nullptr || capacity == 0 || src == nullptr) {
        return false;
    }
    size_t length = strlen(dest);
    if (length >= capacity) {
        return false;
    }
    size_t i = 0;
    while (src[i] != '\0' && length + 1 < capacity) {
        dest[length++] = src[i++];
    }
    dest[length] = '\0';
    return src[i] == '\0';
}

void trim_spaces(char* text) {
    if (text == nullptr) {
        return;
    }
    size_t start = 0;
    while (text[start] == ' ' || text[start] == '\t') {
        ++start;
    }
    size_t length = strlen(text + start);
    memmove(text, text + start, length + 1);
    while (length != 0 &&
           (text[length - 1] == ' ' || text[length - 1] == '\t')) {
        text[--length] = '\0';
    }
}

bool parse_u16_range(const char* begin, const char* end, uint16_t& out) {
    if (begin == nullptr || end == nullptr || begin == end) {
        return false;
    }
    uint32_t value = 0;
    for (const char* cursor = begin; cursor != end; ++cursor) {
        if (!is_digit(*cursor)) {
            return false;
        }
        value = value * 10u + static_cast<uint32_t>(*cursor - '0');
        if (value > 65535u) {
            return false;
        }
    }
    out = static_cast<uint16_t>(value);
    return true;
}

bool parse_decimal(const char* text, size_t& out) {
    if (text == nullptr || *text == '\0') {
        return false;
    }
    size_t value = 0;
    while (*text != '\0') {
        if (!is_digit(*text)) {
            return false;
        }
        value = value * 10u + static_cast<size_t>(*text - '0');
        ++text;
    }
    out = value;
    return true;
}

int parse_decimal_int(const char* text) {
    size_t value = 0;
    if (!parse_decimal(text, value) || value > static_cast<size_t>(0x7FFFFFFF)) {
        return -1;
    }
    return static_cast<int>(value);
}

bool parse_decimal_range(const char* begin, const char* end, int& out) {
    if (begin == nullptr || end == nullptr || begin >= end) {
        return false;
    }
    int value = 0;
    for (const char* cursor = begin; cursor != end; ++cursor) {
        if (!is_digit(*cursor)) {
            return false;
        }
        value = value * 10 + static_cast<int>(*cursor - '0');
    }
    out = value;
    return true;
}

bool parse_hex_size(const char* text, size_t& out) {
    if (text == nullptr || *text == '\0') {
        return false;
    }
    size_t value = 0;
    while (*text != '\0' && *text != ';') {
        char ch = to_lower(*text);
        uint8_t digit = 0;
        if (ch >= '0' && ch <= '9') {
            digit = static_cast<uint8_t>(ch - '0');
        } else if (ch >= 'a' && ch <= 'f') {
            digit = static_cast<uint8_t>(10 + ch - 'a');
        } else {
            return false;
        }
        value = (value << 4) | digit;
        ++text;
    }
    out = value;
    return true;
}

bool parse_url(const char* text, Url& out, UrlParseMode mode) {
    if (text == nullptr || *text == '\0') {
        return false;
    }
    memset(&out, 0, sizeof(out));

    const char* cursor = text;
    if (starts_with_ci(cursor, "http://")) {
        out.scheme = Scheme::Http;
        out.port = 80;
        cursor += 7;
    } else if (starts_with_ci(cursor, "https://")) {
        out.scheme = Scheme::Https;
        out.port = 443;
        cursor += 8;
    } else if (mode == UrlParseMode::DefaultHttps) {
        out.scheme = Scheme::Https;
        out.port = 443;
    } else if (mode == UrlParseMode::DefaultHttp) {
        out.scheme = Scheme::Http;
        out.port = 80;
    } else {
        return false;
    }

    const char* host_begin = cursor;
    while (*cursor != '\0' && *cursor != '/' && *cursor != ':') {
        ++cursor;
    }
    if (cursor == host_begin) {
        return false;
    }
    copy_range(out.host, sizeof(out.host), host_begin, cursor);

    if (*cursor == ':') {
        ++cursor;
        const char* port_begin = cursor;
        while (*cursor != '\0' && *cursor != '/') {
            ++cursor;
        }
        if (!parse_u16_range(port_begin, cursor, out.port) || out.port == 0) {
            return false;
        }
    }

    if (*cursor == '\0') {
        copy_range(out.path, sizeof(out.path), "/", "/" + 1);
        out.query[0] = '\0';
    } else {
        const char* path_begin = cursor;
        while (*cursor != '\0' && *cursor != '?') {
            ++cursor;
        }
        copy_range(out.path, sizeof(out.path), path_begin, cursor);
        if (*cursor == '?') {
            ++cursor;
            copy_range(out.query, sizeof(out.query), cursor, cursor + strlen(cursor));
        } else {
            out.query[0] = '\0';
        }
    }
    return true;
}

bool parse_ipv4_component(const char* begin, const char* end, uint8_t& out) {
    if (begin == end) {
        return false;
    }
    uint32_t value = 0;
    for (const char* cursor = begin; cursor != end; ++cursor) {
        if (!is_digit(*cursor)) {
            return false;
        }
        value = value * 10u + static_cast<uint32_t>(*cursor - '0');
        if (value > 255u) {
            return false;
        }
    }
    out = static_cast<uint8_t>(value);
    return true;
}

bool parse_ipv4_literal(const char* text, uint8_t out[4]) {
    if (text == nullptr || *text == '\0' || out == nullptr) {
        return false;
    }
    const char* component = text;
    for (size_t i = 0; i < 4; ++i) {
        const char* cursor = component;
        while (*cursor != '\0' && *cursor != '.') {
            ++cursor;
        }
        if (!parse_ipv4_component(component, cursor, out[i])) {
            return false;
        }
        if (i != 3) {
            if (*cursor != '.') {
                return false;
            }
            component = cursor + 1;
        } else if (*cursor != '\0') {
            return false;
        }
    }
    return true;
}

void append_port(char* out, size_t out_size, uint16_t port) {
    size_t length = strlen(out);
    if (length + 1 >= out_size) {
        return;
    }
    out[length++] = ':';
    char digits[8];
    size_t count = 0;
    do {
        digits[count++] = static_cast<char>('0' + (port % 10u));
        port = static_cast<uint16_t>(port / 10u);
    } while (port != 0 && count < sizeof(digits));
    while (count != 0 && length + 1 < out_size) {
        out[length++] = digits[--count];
    }
    out[length] = '\0';
}

bool url_to_string(const Url& url, char* out, size_t out_size) {
    if (out == nullptr || out_size == 0) {
        return false;
    }
    const char* scheme = url.scheme == Scheme::Https ? "https://" : "http://";
    if (strlen(scheme) + strlen(url.host) + strlen(url.path) + 8 >= out_size) {
        return false;
    }
    strlcpy(out, scheme, out_size);
    if (!append_cstr(out, out_size, url.host)) {
        return false;
    }
    bool need_port = (url.scheme == Scheme::Http && url.port != 80) ||
                     (url.scheme == Scheme::Https && url.port != 443);
    if (need_port) {
        append_port(out, out_size, url.port);
    }
    if (!append_cstr(out, out_size, url.path)) {
        return false;
    }
    if (url.query[0] != '\0') {
        return append_cstr(out, out_size, "?") &&
               append_cstr(out, out_size, url.query);
    }
    return true;
}

bool build_redirect_url(const Url& current,
                        const char* location,
                        char* out,
                        size_t out_size) {
    if (location == nullptr || out == nullptr || out_size == 0 || *location == '\0') {
        return false;
    }
    if (starts_with_ci(location, "http://") || starts_with_ci(location, "https://")) {
        strlcpy(out, location, out_size);
        return strlen(location) < out_size;
    }
    if (location[0] == '/' && location[1] == '/') {
        const char* scheme = current.scheme == Scheme::Https ? "https:" : "http:";
        if (strlen(scheme) + strlen(location) >= out_size) {
            return false;
        }
        strlcpy(out, scheme, out_size);
        return append_cstr(out, out_size, location);
    }
    if (location[0] == '/') {
        Url root = current;
        copy_range(root.path, sizeof(root.path), location, location + strlen(location));
        root.query[0] = '\0';
        return url_to_string(root, out, out_size);
    }

    Url relative = current;
    const char* slash = relative.path + strlen(relative.path);
    while (slash > relative.path && slash[-1] != '/') {
        --slash;
    }
    char base[kMaxPath];
    copy_range(base, sizeof(base), relative.path, slash);
    if (strlen(base) + strlen(location) >= sizeof(relative.path)) {
        return false;
    }
    copy_range(relative.path, sizeof(relative.path), base, base + strlen(base));
    if (!append_cstr(relative.path, sizeof(relative.path), location)) {
        return false;
    }
    relative.query[0] = '\0';
    return url_to_string(relative, out, out_size);
}

void init_response_meta(ResponseMeta& meta) {
    meta.status_code = 0;
    meta.chunked = false;
    meta.have_content_length = false;
    meta.content_length = 0;
    meta.is_html = false;
    meta.is_text = false;
    meta.location[0] = '\0';
}

bool read_response_headers(void* context, ReadLineFn read_line, ResponseMeta& meta) {
    if (read_line == nullptr) {
        return false;
    }
    init_response_meta(meta);
    char line[1024];
    if (!read_line(context, line, sizeof(line))) {
        return false;
    }
    int first_space = find_char(line, ' ');
    if (first_space >= 0) {
        const char* status_begin = line + first_space + 1;
        const char* status_end = status_begin;
        while (*status_end != '\0' && *status_end != ' ') {
            ++status_end;
        }
        int status_code = 0;
        if (parse_decimal_range(status_begin, status_end, status_code)) {
            meta.status_code = status_code;
        }
    }
    for (;;) {
        if (!read_line(context, line, sizeof(line))) {
            return false;
        }
        if (line[0] == '\0') {
            return true;
        }
        int colon = find_char(line, ':');
        if (colon <= 0) {
            continue;
        }
        line[colon] = '\0';
        char* value = line + colon + 1;
        trim_spaces(value);
        if (starts_with_ci(line, "content-type")) {
            if (starts_with_ci(value, "text/html") ||
                starts_with_ci(value, "application/xhtml+xml")) {
                meta.is_html = true;
                meta.is_text = true;
            } else if (starts_with_ci(value, "text/") ||
                       starts_with_ci(value, "application/json") ||
                       starts_with_ci(value, "application/xml")) {
                meta.is_text = true;
            }
        } else if (starts_with_ci(line, "content-length")) {
            size_t parsed = 0;
            if (parse_decimal(value, parsed)) {
                meta.have_content_length = true;
                meta.content_length = parsed;
            }
        } else if (starts_with_ci(line, "transfer-encoding")) {
            if (starts_with_ci(value, "chunked")) {
                meta.chunked = true;
            }
        } else if (starts_with_ci(line, "location")) {
            copy_range(meta.location, sizeof(meta.location), value,
                       value + strlen(value));
        }
    }
}

int hex_value(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return 10 + ch - 'a';
    if (ch >= 'A' && ch <= 'F') return 10 + ch - 'A';
    return -1;
}

size_t percent_decode(const char* input, size_t input_length,
                      char* output, size_t output_capacity) {
    if (output == nullptr || output_capacity == 0) {
        return 0;
    }
    size_t out = 0;
    for (size_t i = 0; i < input_length && out + 1 < output_capacity; ++i) {
        unsigned char ch = static_cast<unsigned char>(input[i]);
        if (ch == '%' && i + 2 < input_length) {
            int high = hex_value(input[i + 1]);
            int low = hex_value(input[i + 2]);
            if (high >= 0 && low >= 0) {
                ch = static_cast<unsigned char>((high << 4) | low);
                i += 2;
            }
        } else if (ch == '+') {
            ch = ' ';
        }
        output[out++] = static_cast<char>(ch);
    }
    output[out] = '\0';
    return out;
}

bool percent_encode(const char* input, char* output, size_t output_capacity) {
    if (input == nullptr || output == nullptr || output_capacity == 0) {
        return false;
    }
    static const char kHex[] = "0123456789ABCDEF";
    size_t out = 0;
    for (size_t i = 0; input[i] != '\0'; ++i) {
        unsigned char ch = static_cast<unsigned char>(input[i]);
        bool unreserved =
            (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
            (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' ||
            ch == '.' || ch == '~' || ch == '/';
        if (unreserved) {
            if (out + 1 >= output_capacity) return false;
            output[out++] = static_cast<char>(ch);
        } else {
            if (out + 3 >= output_capacity) return false;
            output[out++] = '%';
            output[out++] = kHex[ch >> 4];
            output[out++] = kHex[ch & 0x0F];
        }
    }
    output[out] = '\0';
    return true;
}

void init_request(Request& request) {
    memset(&request, 0, sizeof(request));
    copy_range(request.method, sizeof(request.method), "GET", "GET" + 3);
    copy_range(request.target, sizeof(request.target), "/", "/" + 1);
    copy_range(request.version, sizeof(request.version), "HTTP/1.1",
               "HTTP/1.1" + 8);
}

void init_response(Response& response) {
    memset(&response, 0, sizeof(response));
    copy_range(response.version, sizeof(response.version), "HTTP/1.1",
               "HTTP/1.1" + 8);
    copy_range(response.reason, sizeof(response.reason), "OK", "OK" + 2);
    response.status_code = 200;
}

bool add_header(Header* headers, size_t capacity, size_t& count,
                const char* name, const char* value) {
    if (headers == nullptr || name == nullptr || value == nullptr ||
        count >= capacity) {
        return false;
    }
    copy_range(headers[count].name, sizeof(headers[count].name), name,
               name + strlen(name));
    copy_range(headers[count].value, sizeof(headers[count].value), value,
               value + strlen(value));
    ++count;
    return true;
}

const char* find_header(const Header* headers, size_t count, const char* name) {
    if (headers == nullptr || name == nullptr) {
        return nullptr;
    }
    for (size_t i = 0; i < count; ++i) {
        if (starts_with_ci(headers[i].name, name) &&
            headers[i].name[strlen(name)] == '\0') {
            return headers[i].value;
        }
    }
    return nullptr;
}

bool parse_request_line(const char* line, Request& out) {
    if (line == nullptr) {
        return false;
    }
    const char* method_end = line;
    while (*method_end != '\0' && *method_end != ' ') ++method_end;
    if (method_end == line || *method_end != ' ') return false;
    copy_range(out.method, sizeof(out.method), line, method_end);
    const char* target = method_end + 1;
    const char* target_end = target;
    while (*target_end != '\0' && *target_end != ' ') ++target_end;
    if (target_end == target || *target_end != ' ') return false;
    copy_range(out.target, sizeof(out.target), target, target_end);
    const char* version = target_end + 1;
    if (*version == '\0') return false;
    copy_range(out.version, sizeof(out.version), version, version + strlen(version));
    return true;
}

bool parse_status_line(const char* line, Response& out) {
    if (line == nullptr) {
        return false;
    }
    const char* version_end = line;
    while (*version_end != '\0' && *version_end != ' ') ++version_end;
    if (version_end == line || *version_end != ' ') return false;
    copy_range(out.version, sizeof(out.version), line, version_end);
    const char* status_begin = version_end + 1;
    const char* status_end = status_begin;
    while (*status_end != '\0' && *status_end != ' ') ++status_end;
    int status = 0;
    if (!parse_decimal_range(status_begin, status_end, status)) {
        return false;
    }
    out.status_code = status;
    if (*status_end == ' ') {
        const char* reason = status_end + 1;
        copy_range(out.reason, sizeof(out.reason), reason, reason + strlen(reason));
    } else {
        out.reason[0] = '\0';
    }
    return true;
}

bool parse_header_line(const char* line, Header& out) {
    if (line == nullptr) {
        return false;
    }
    int colon = find_char(line, ':');
    if (colon <= 0) {
        return false;
    }
    copy_range(out.name, sizeof(out.name), line, line + colon);
    const char* value = line + colon + 1;
    while (*value == ' ' || *value == '\t') ++value;
    copy_range(out.value, sizeof(out.value), value, value + strlen(value));
    trim_spaces(out.name);
    trim_spaces(out.value);
    return true;
}

bool read_headers(void* context, ReadLineFn read_line,
                  Header* headers, size_t capacity, size_t& count) {
    if (read_line == nullptr || headers == nullptr) {
        return false;
    }
    count = 0;
    char line[1024];
    for (;;) {
        if (!read_line(context, line, sizeof(line))) {
            return false;
        }
        if (line[0] == '\0') {
            return true;
        }
        if (count >= capacity) {
            return false;
        }
        if (!parse_header_line(line, headers[count])) {
            continue;
        }
        ++count;
    }
}

bool read_request_headers(void* context, ReadLineFn read_line, Request& request) {
    if (read_line == nullptr) {
        return false;
    }
    char line[1024];
    if (!read_line(context, line, sizeof(line))) {
        return false;
    }
    if (!parse_request_line(line, request)) {
        return false;
    }
    return read_headers(context, read_line, request.headers,
                        kMaxHeaders, request.header_count);
}

bool write_all(ByteStream& stream, const void* data, size_t length) {
    if (stream.write == nullptr) {
        return false;
    }
    return stream.write(stream.context, data, length);
}

bool write_cstr(ByteStream& stream, const char* text) {
    if (text == nullptr) {
        return false;
    }
    return write_all(stream, text, strlen(text));
}

bool write_decimal(ByteStream& stream, size_t value) {
    char digits[24];
    size_t count = 0;
    do {
        digits[count++] = static_cast<char>('0' + (value % 10));
        value /= 10;
    } while (value != 0 && count < sizeof(digits));
    char text[24];
    size_t length = 0;
    while (count != 0) {
        text[length++] = digits[--count];
    }
    return write_all(stream, text, length);
}

bool write_request(ByteStream& stream, const Request& request) {
    if (!write_cstr(stream, request.method) ||
        !write_cstr(stream, " ") ||
        !write_cstr(stream, request.target) ||
        !write_cstr(stream, " ") ||
        !write_cstr(stream, request.version) ||
        !write_cstr(stream, "\r\n")) {
        return false;
    }
    for (size_t i = 0; i < request.header_count; ++i) {
        if (!write_cstr(stream, request.headers[i].name) ||
            !write_cstr(stream, ": ") ||
            !write_cstr(stream, request.headers[i].value) ||
            !write_cstr(stream, "\r\n")) {
            return false;
        }
    }
    if (!write_cstr(stream, "\r\n")) {
        return false;
    }
    if (request.body != nullptr && request.body_length != 0) {
        return write_all(stream, request.body, request.body_length);
    }
    return true;
}

bool write_response(ByteStream& stream, const Response& response,
                    const void* body, size_t body_length) {
    if (!write_cstr(stream, response.version) ||
        !write_cstr(stream, " ") ||
        !write_decimal(stream, static_cast<size_t>(response.status_code)) ||
        !write_cstr(stream, " ") ||
        !write_cstr(stream, response.reason) ||
        !write_cstr(stream, "\r\n")) {
        return false;
    }
    bool have_length = false;
    for (size_t i = 0; i < response.header_count; ++i) {
        if (starts_with_ci(response.headers[i].name, "content-length")) {
            have_length = true;
        }
        if (!write_cstr(stream, response.headers[i].name) ||
            !write_cstr(stream, ": ") ||
            !write_cstr(stream, response.headers[i].value) ||
            !write_cstr(stream, "\r\n")) {
            return false;
        }
    }
    if (!have_length) {
        if (!write_cstr(stream, "Content-Length: ") ||
            !write_decimal(stream, body_length) ||
            !write_cstr(stream, "\r\n")) {
            return false;
        }
    }
    if (!write_cstr(stream, "\r\n")) {
        return false;
    }
    if (body != nullptr && body_length != 0) {
        return write_all(stream, body, body_length);
    }
    return true;
}

bool read_content_length_body(ByteStream& stream, size_t length,
                              uint8_t* out, size_t capacity, size_t& got) {
    got = 0;
    if (stream.read == nullptr || out == nullptr) {
        return false;
    }
    if (length > capacity) {
        length = capacity;
    }
    while (got < length) {
        size_t n = 0;
        if (!stream.read(stream.context, out + got, length - got, n) || n == 0) {
            return false;
        }
        got += n;
    }
    return true;
}

bool read_chunked_body(void* context, ReadLineFn read_line, ByteStream& stream,
                       uint8_t* out, size_t capacity, size_t& got) {
    got = 0;
    if (read_line == nullptr || stream.read == nullptr || out == nullptr) {
        return false;
    }
    char line[64];
    for (;;) {
        if (!read_line(context, line, sizeof(line))) {
            return false;
        }
        size_t chunk = 0;
        if (!parse_hex_size(line, chunk)) {
            return false;
        }
        if (chunk == 0) {
            (void)read_line(context, line, sizeof(line));
            return true;
        }
        if (got + chunk > capacity) {
            return false;
        }
        size_t remaining = chunk;
        while (remaining != 0) {
            size_t n = 0;
            if (!stream.read(stream.context, out + got, remaining, n) || n == 0) {
                return false;
            }
            got += n;
            remaining -= n;
        }
        if (!read_line(context, line, sizeof(line))) {
            return false;
        }
    }
}

bool write_hex(ByteStream& stream, size_t value) {
    char digits[16];
    size_t count = 0;
    static const char kHex[] = "0123456789abcdef";
    do {
        digits[count++] = kHex[value & 0xF];
        value >>= 4;
    } while (value != 0 && count < sizeof(digits));
    char text[16];
    size_t length = 0;
    while (count != 0) {
        text[length++] = digits[--count];
    }
    return write_all(stream, text, length);
}

bool write_chunk(ByteStream& stream, const void* data, size_t length) {
    return write_hex(stream, length) &&
           write_cstr(stream, "\r\n") &&
           write_all(stream, data, length) &&
           write_cstr(stream, "\r\n");
}

bool write_chunk_end(ByteStream& stream) {
    return write_cstr(stream, "0\r\n\r\n");
}

void response_to_meta(const Response& response, ResponseMeta& meta) {
    init_response_meta(meta);
    meta.status_code = response.status_code;
    meta.chunked = response.chunked;
    meta.have_content_length = response.have_content_length;
    meta.content_length = response.content_length;
    const char* type = find_header(response.headers, response.header_count,
                                   "content-type");
    if (type != nullptr) {
        if (starts_with_ci(type, "text/html") ||
            starts_with_ci(type, "application/xhtml+xml")) {
            meta.is_html = true;
            meta.is_text = true;
        } else if (starts_with_ci(type, "text/") ||
                   starts_with_ci(type, "application/json") ||
                   starts_with_ci(type, "application/xml")) {
            meta.is_text = true;
        }
    }
    const char* location = find_header(response.headers, response.header_count,
                                       "location");
    if (location != nullptr) {
        copy_range(meta.location, sizeof(meta.location), location,
                   location + strlen(location));
    }
}

}  // namespace http
