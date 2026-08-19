#include <neutrino/http.hpp>
#include "../../libnet/src/http.cpp"

#include <stdio.h>
#include <string.h>

namespace {

struct MemoryStream {
    const uint8_t* input;
    size_t input_length;
    size_t input_offset;
    uint8_t output[2048];
    size_t output_length;
};

bool mem_write(void* context, const void* data, size_t length) {
    auto* stream = static_cast<MemoryStream*>(context);
    if (stream->output_length + length > sizeof(stream->output)) {
        return false;
    }
    memcpy(stream->output + stream->output_length, data, length);
    stream->output_length += length;
    return true;
}

bool mem_read(void* context, void* data, size_t length, size_t& got) {
    auto* stream = static_cast<MemoryStream*>(context);
    size_t available = stream->input_length - stream->input_offset;
    if (available == 0) {
        got = 0;
        return false;
    }
    if (length > available) {
        length = available;
    }
    memcpy(data, stream->input + stream->input_offset, length);
    stream->input_offset += length;
    got = length;
    return true;
}

bool mem_read_line(void* context, char* out, size_t out_size) {
    auto* stream = static_cast<MemoryStream*>(context);
    size_t length = 0;
    while (stream->input_offset < stream->input_length && length + 1 < out_size) {
        char ch = static_cast<char>(stream->input[stream->input_offset++]);
        if (ch == '\r') {
            continue;
        }
        if (ch == '\n') {
            out[length] = '\0';
            return true;
        }
        out[length++] = ch;
    }
    return false;
}

int fail(const char* message) {
    fprintf(stderr, "http_test: %s\n", message);
    return 1;
}

}  // namespace

int main() {
    using namespace http;
    Url url{};
    if (!parse_url("http://example.com:8080/foo?bar=1", url,
                   UrlParseMode::RequireScheme) ||
        url.port != 8080 || strcmp(url.host, "example.com") != 0 ||
        strcmp(url.path, "/foo") != 0 || strcmp(url.query, "bar=1") != 0) {
        return fail("url parse");
    }

    Request request{};
    if (!parse_request_line("GET /index.html HTTP/1.1", request) ||
        strcmp(request.method, "GET") != 0 ||
        strcmp(request.target, "/index.html") != 0) {
        return fail("request line");
    }

    MemoryStream memory{};
    ByteStream stream{};
    stream.context = &memory;
    stream.write = mem_write;
    stream.read = mem_read;
    init_request(request);
    add_header(request.headers, kMaxHeaders, request.header_count, "Host",
               "example.com");
    if (!write_request(stream, request)) {
        return fail("write request");
    }
    memory.output[memory.output_length] = '\0';
    if (strstr(reinterpret_cast<char*>(memory.output), "GET / HTTP/1.1") ==
            nullptr ||
        strstr(reinterpret_cast<char*>(memory.output), "Host: example.com") ==
            nullptr) {
        return fail("serialized request");
    }

    const char headers[] =
        "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n"
        "Transfer-Encoding: chunked\r\n\r\n";
    memory.input = reinterpret_cast<const uint8_t*>(headers);
    memory.input_length = sizeof(headers) - 1;
    memory.input_offset = 0;
    ResponseMeta meta{};
    if (!read_response_headers(&memory, mem_read_line, meta) ||
        meta.status_code != 200 || !meta.have_content_length ||
        meta.content_length != 5 || !meta.chunked) {
        return fail("response headers");
    }

    char decoded[16];
    percent_decode("%2Ftmp", 6, decoded, sizeof(decoded));
    if (strcmp(decoded, "/tmp") != 0) {
        return fail("percent decode");
    }

    printf("http_test: ok\n");
    return 0;
}
