#pragma once

#include <stddef.h>
#include <stdint.h>

#include <neutrino/net.hpp>

namespace http {

constexpr size_t kMaxUrl = 512;
constexpr size_t kMaxHost = 256;
constexpr size_t kMaxPath = 512;
constexpr size_t kMaxQuery = 256;
constexpr size_t kMaxLocation = 512;
constexpr size_t kMaxMethod = 16;
constexpr size_t kMaxReason = 64;
constexpr size_t kMaxHeaderName = 64;
constexpr size_t kMaxHeaderValue = 256;
constexpr size_t kMaxHeaders = 24;
constexpr size_t kMaxVersion = 16;

enum class Scheme : uint8_t {
    Http,
    Https,
};

enum class UrlParseMode : uint8_t {
    RequireScheme,
    DefaultHttps,
    DefaultHttp,
};

struct Url {
    Scheme scheme;
    char host[kMaxHost];
    char path[kMaxPath];
    char query[kMaxQuery];
    uint16_t port;
};

struct Header {
    char name[kMaxHeaderName];
    char value[kMaxHeaderValue];
};

struct Request {
    char method[kMaxMethod];
    char target[kMaxPath];
    char version[kMaxVersion];
    Header headers[kMaxHeaders];
    size_t header_count;
    const uint8_t* body;
    size_t body_length;
};

struct Response {
    char version[kMaxVersion];
    int status_code;
    char reason[kMaxReason];
    Header headers[kMaxHeaders];
    size_t header_count;
    bool chunked;
    bool have_content_length;
    size_t content_length;
};

struct ResponseMeta {
    int status_code;
    bool chunked;
    bool have_content_length;
    size_t content_length;
    bool is_html;
    bool is_text;
    char location[kMaxLocation];
};

struct ByteStream {
    void* context;
    bool (*read)(void* context, void* data, size_t length, size_t& got);
    bool (*write)(void* context, const void* data, size_t length);
};

using ReadLineFn = bool (*)(void* context, char* out, size_t out_size);

struct ConnectionIO {
    net::Connection* connection;
};

bool is_digit(char ch);
char to_lower(char ch);
bool starts_with_ci(const char* text, const char* prefix);
int find_char(const char* text, char ch);
void copy_range(char* dest, size_t capacity, const char* begin, const char* end);
bool append_cstr(char* dest, size_t capacity, const char* src);
void trim_spaces(char* text);

bool parse_u16_range(const char* begin, const char* end, uint16_t& out);
bool parse_decimal(const char* text, size_t& out);
int parse_decimal_int(const char* text);
bool parse_decimal_range(const char* begin, const char* end, int& out);
bool parse_hex_size(const char* text, size_t& out);

int hex_value(char ch);
size_t percent_decode(const char* input, size_t input_length,
                      char* output, size_t output_capacity);
bool percent_encode(const char* input, char* output, size_t output_capacity);

bool parse_url(const char* text, Url& out, UrlParseMode mode);
bool parse_ipv4_literal(const char* text, uint8_t out[4]);
bool url_to_string(const Url& url, char* out, size_t out_size);
bool build_redirect_url(const Url& current,
                        const char* location,
                        char* out,
                        size_t out_size);

void init_request(Request& request);
void init_response(Response& response);
void init_response_meta(ResponseMeta& meta);
bool add_header(Header* headers, size_t capacity, size_t& count,
                const char* name, const char* value);
const char* find_header(const Header* headers, size_t count, const char* name);

bool parse_request_line(const char* line, Request& out);
bool parse_status_line(const char* line, Response& out);
bool parse_header_line(const char* line, Header& out);
bool read_headers(void* context, ReadLineFn read_line,
                  Header* headers, size_t capacity, size_t& count);
bool read_response_headers(void* context, ReadLineFn read_line, ResponseMeta& meta);
bool read_request_headers(void* context, ReadLineFn read_line, Request& request);

bool write_all(ByteStream& stream, const void* data, size_t length);
bool write_cstr(ByteStream& stream, const char* text);
bool write_request(ByteStream& stream, const Request& request);
bool write_response(ByteStream& stream, const Response& response,
                    const void* body, size_t body_length);

bool read_content_length_body(ByteStream& stream, size_t length,
                              uint8_t* out, size_t capacity, size_t& got);
bool read_chunked_body(void* context, ReadLineFn read_line, ByteStream& stream,
                       uint8_t* out, size_t capacity, size_t& got);
bool write_chunk(ByteStream& stream, const void* data, size_t length);
bool write_chunk_end(ByteStream& stream);

void response_to_meta(const Response& response, ResponseMeta& meta);

bool connection_write(void* context, const void* data, size_t length);
bool connection_read(void* context, void* data, size_t length, size_t& got);
bool connection_read_line(void* context, char* out, size_t out_size);
ByteStream connection_stream(ConnectionIO& io);

bool write_request(net::Connection& connection, const Request& request);
bool write_response(net::Connection& connection, const Response& response,
                    const void* body, size_t body_length);
bool write_error(net::Connection& connection,
                 const char* status,
                 const char* content_type,
                 const char* body);
bool read_response_headers(net::Connection& connection, ResponseMeta& meta);
bool headers_complete(const char* data, size_t length);
bool parse_request_buffer(char* data, size_t length, Request& request);

bool request(const char* url_text,
             const char* method,
             const void* body,
             size_t body_length,
             ResponseMeta& meta,
             ByteStream* response_body);
bool get(const char* url, ResponseMeta& meta, ByteStream* response_body);
bool post(const char* url,
          const void* body,
          size_t body_length,
          ResponseMeta& meta,
          ByteStream* response_body);

}  // namespace http
