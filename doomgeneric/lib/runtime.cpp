#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "descriptors.hpp"
#include "syscall.hpp"

int errno = 0;

namespace {

struct Allocation {
    size_t mapped_size;
    size_t requested_size;
};

constexpr size_t kPageSize = 4096;
constexpr size_t kMaxExitFunctions = 32;
void (*g_exit_functions[kMaxExitFunctions])(void);
size_t g_exit_function_count = 0;

size_t round_pages(size_t size) {
    if (size > SIZE_MAX - (kPageSize - 1)) return 0;
    return (size + kPageSize - 1) & ~(kPageSize - 1);
}

int digit_value(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'z') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'Z') return ch - 'A' + 10;
    return -1;
}

unsigned long parse_unsigned(const char* text, char** end, int base,
                             bool* negative) {
    const char* cursor = text;
    while (isspace(static_cast<unsigned char>(*cursor))) ++cursor;
    bool neg = false;
    if (*cursor == '+' || *cursor == '-') {
        neg = *cursor == '-';
        ++cursor;
    }
    if ((base == 0 || base == 16) && cursor[0] == '0' &&
        (cursor[1] == 'x' || cursor[1] == 'X')) {
        base = 16;
        cursor += 2;
    } else if (base == 0) {
        base = cursor[0] == '0' ? 8 : 10;
    }
    const char* first = cursor;
    unsigned long value = 0;
    while (*cursor != '\0') {
        int digit = digit_value(*cursor);
        if (digit < 0 || digit >= base) break;
        value = value * static_cast<unsigned long>(base) +
                static_cast<unsigned long>(digit);
        ++cursor;
    }
    if (end != nullptr) *end = const_cast<char*>(cursor == first ? text : cursor);
    if (negative != nullptr) *negative = neg;
    return value;
}

bool get_file_size(const char* path, uint64_t& size) {
    char directory[256]{};
    const char* name = path;
    size_t slash = SIZE_MAX;
    for (size_t i = 0; path[i] != '\0'; ++i) {
        if (path[i] == '/') slash = i;
    }
    if (slash == SIZE_MAX) {
        directory[0] = '.';
        directory[1] = '\0';
    } else {
        size_t length = slash == 0 ? 1 : slash;
        if (length >= sizeof(directory)) return false;
        memcpy(directory, path, length);
        directory[length] = '\0';
        name = path + slash + 1;
    }
    if (*name == '\0') return false;
    long opened = directory_open(directory);
    if (opened < 0) return false;
    bool found = false;
    DirEntry entry{};
    while (directory_read(static_cast<uint32_t>(opened), &entry) > 0) {
        if ((entry.flags & DIR_ENTRY_FLAG_DIRECTORY) == 0 &&
            strcmp(entry.name, name) == 0) {
            size = entry.size;
            found = true;
            break;
        }
    }
    directory_close(static_cast<uint32_t>(opened));
    return found;
}

enum class FileKind : uint8_t { Input, Output, Console };

}  // namespace

struct NeutrinoFile {
    FileKind kind;
    uint8_t* data;
    size_t size;
    size_t capacity;
    size_t position;
    char* path;
    uint32_t descriptor;
    bool error;
};

namespace {

NeutrinoFile g_stdout{FileKind::Console, nullptr, 0, 0, 0, nullptr,
                      UINT32_MAX, false};
NeutrinoFile g_stderr{FileKind::Console, nullptr, 0, 0, 0, nullptr,
                      UINT32_MAX, false};
NeutrinoFile g_stdin{FileKind::Input, nullptr, 0, 0, 0, nullptr,
                     UINT32_MAX, false};

uint32_t console_handle(NeutrinoFile* stream) {
    if (stream->descriptor != UINT32_MAX) return stream->descriptor;
    uint32_t index = stream == &g_stderr ? 2 : 1;
    long descriptor = process_get_standard_descriptor(index);
    if (descriptor < 0) {
        descriptor = descriptor_open(
            static_cast<uint32_t>(descriptor_defs::Type::Console), 0);
    }
    if (descriptor >= 0) {
        stream->descriptor = static_cast<uint32_t>(descriptor);
    }
    return stream->descriptor;
}

bool ensure_capacity(NeutrinoFile* stream, size_t required) {
    if (required <= stream->capacity) return true;
    size_t capacity = stream->capacity == 0 ? 256 : stream->capacity;
    while (capacity < required) {
        if (capacity > SIZE_MAX / 2) return false;
        capacity *= 2;
    }
    void* replacement = realloc(stream->data, capacity);
    if (replacement == nullptr) return false;
    stream->data = static_cast<uint8_t*>(replacement);
    stream->capacity = capacity;
    return true;
}

struct FormatOutput {
    char* data;
    size_t capacity;
    size_t length;
};

void format_char(FormatOutput& out, char ch) {
    if (out.capacity != 0 && out.length + 1 < out.capacity) {
        out.data[out.length] = ch;
    }
    ++out.length;
}

void format_repeat(FormatOutput& out, char ch, size_t count) {
    while (count-- != 0) format_char(out, ch);
}

void format_span(FormatOutput& out, const char* text, size_t length) {
    for (size_t i = 0; i < length; ++i) format_char(out, text[i]);
}

void format_integer(FormatOutput& out, uint64_t value, bool negative,
                    unsigned base, bool upper, int width, int precision,
                    bool left, bool zero, bool alternate) {
    char digits[32];
    size_t count = 0;
    do {
        unsigned digit = static_cast<unsigned>(value % base);
        digits[count++] = static_cast<char>(
            digit < 10 ? '0' + digit : (upper ? 'A' : 'a') + digit - 10);
        value /= base;
    } while (value != 0);
    while (static_cast<int>(count) < precision) digits[count++] = '0';

    char prefix[3];
    size_t prefix_length = 0;
    if (negative) prefix[prefix_length++] = '-';
    if (alternate && base == 16) {
        prefix[prefix_length++] = '0';
        prefix[prefix_length++] = upper ? 'X' : 'x';
    } else if (alternate && base == 8 && digits[count - 1] != '0') {
        prefix[prefix_length++] = '0';
    }
    size_t total = prefix_length + count;
    size_t padding = width > static_cast<int>(total)
                         ? static_cast<size_t>(width) - total
                         : 0;
    if (!left && !zero) format_repeat(out, ' ', padding);
    format_span(out, prefix, prefix_length);
    if (!left && zero) format_repeat(out, '0', padding);
    while (count != 0) format_char(out, digits[--count]);
    if (left) format_repeat(out, ' ', padding);
}

}  // namespace

FILE* stdin = &g_stdin;
FILE* stdout = &g_stdout;
FILE* stderr = &g_stderr;

extern "C" void* malloc(size_t size) {
    if (size == 0) size = 1;
    if (size > SIZE_MAX - sizeof(Allocation)) return nullptr;
    size_t mapped = round_pages(size + sizeof(Allocation));
    if (mapped == 0) return nullptr;
    void* memory = map_anonymous(mapped, MAP_WRITE);
    if (memory == nullptr) return nullptr;
    auto* header = static_cast<Allocation*>(memory);
    header->mapped_size = mapped;
    header->requested_size = size;
    return header + 1;
}

extern "C" void* calloc(size_t count, size_t size) {
    if (count != 0 && size > SIZE_MAX / count) return nullptr;
    size_t bytes = count * size;
    void* memory = malloc(bytes);
    if (memory != nullptr) memset(memory, 0, bytes);
    return memory;
}

extern "C" void free(void* memory) {
    if (memory == nullptr) return;
    auto* header = static_cast<Allocation*>(memory) - 1;
    (void)unmap(header, header->mapped_size);
}

extern "C" void* realloc(void* memory, size_t size) {
    if (memory == nullptr) return malloc(size);
    if (size == 0) {
        free(memory);
        return nullptr;
    }
    auto* header = static_cast<Allocation*>(memory) - 1;
    if (size <= header->requested_size) {
        header->requested_size = size;
        return memory;
    }
    void* replacement = malloc(size);
    if (replacement == nullptr) return nullptr;
    memcpy(replacement, memory, header->requested_size);
    free(memory);
    return replacement;
}

extern "C" int atoi(const char* text) {
    return static_cast<int>(strtol(text, nullptr, 10));
}

extern "C" long strtol(const char* text, char** end, int base) {
    bool negative = false;
    unsigned long value = parse_unsigned(text, end, base, &negative);
    return negative ? -static_cast<long>(value) : static_cast<long>(value);
}

extern "C" unsigned long strtoul(const char* text, char** end, int base) {
    bool negative = false;
    unsigned long value = parse_unsigned(text, end, base, &negative);
    return negative ? 0ul - value : value;
}

extern "C" char* getenv(const char*) {
    return nullptr;
}

extern "C" char* strdup(const char* text) {
    if (text == nullptr) return nullptr;
    size_t length = strlen(text) + 1;
    auto* copy = static_cast<char*>(malloc(length));
    if (copy != nullptr) memcpy(copy, text, length);
    return copy;
}

extern "C" int abs(int value) {
    return value < 0 ? -value : value;
}

extern "C" int atexit(void (*function)(void)) {
    if (function == nullptr || g_exit_function_count == kMaxExitFunctions) {
        return -1;
    }
    g_exit_functions[g_exit_function_count++] = function;
    return 0;
}

extern "C" [[noreturn]] void exit(int status) {
    while (g_exit_function_count != 0) {
        void (*function)(void) = g_exit_functions[--g_exit_function_count];
        function();
    }
    ::exit(static_cast<uint16_t>(status));
}

extern "C" [[noreturn]] void abort() {
    ::exit(134);
}

extern "C" int remove(const char* path) {
    return file_remove(path) < 0 ? -1 : 0;
}

extern "C" int rename(const char*, const char*) {
    return -1;
}

extern "C" int system(const char*) {
    return -1;
}

extern "C" int mkdir(const char* path, int) {
    return directory_create(path) < 0 ? -1 : 0;
}

extern "C" int isatty(int fd) {
    return fd >= 0 && fd <= 2;
}

extern "C" int isalpha(int ch) {
    ch = static_cast<unsigned char>(ch);
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
}

extern "C" int isalnum(int ch) {
    return isalpha(ch) || isdigit(ch);
}

extern "C" int isdigit(int ch) {
    ch = static_cast<unsigned char>(ch);
    return ch >= '0' && ch <= '9';
}

extern "C" int islower(int ch) {
    ch = static_cast<unsigned char>(ch);
    return ch >= 'a' && ch <= 'z';
}

extern "C" int isupper(int ch) {
    ch = static_cast<unsigned char>(ch);
    return ch >= 'A' && ch <= 'Z';
}

extern "C" int tolower(int ch) {
    return isupper(ch) ? ch - 'A' + 'a' : ch;
}

extern "C" int toupper(int ch) {
    return islower(ch) ? ch - 'a' + 'A' : ch;
}

extern "C" char* strcpy(char* dest, const char* src) {
    char* result = dest;
    while ((*dest++ = *src++) != '\0') {}
    return result;
}

extern "C" char* strncpy(char* dest, const char* src, size_t count) {
    size_t i = 0;
    while (i < count && src[i] != '\0') {
        dest[i] = src[i];
        ++i;
    }
    while (i < count) dest[i++] = '\0';
    return dest;
}

extern "C" char* strcat(char* dest, const char* src) {
    strcpy(dest + strlen(dest), src);
    return dest;
}

extern "C" char* strchr(const char* text, int ch) {
    do {
        if (*text == static_cast<char>(ch)) return const_cast<char*>(text);
    } while (*text++ != '\0');
    return nullptr;
}

extern "C" char* strrchr(const char* text, int ch) {
    const char* found = nullptr;
    do {
        if (*text == static_cast<char>(ch)) found = text;
    } while (*text++ != '\0');
    return const_cast<char*>(found);
}

extern "C" char* strstr(const char* haystack, const char* needle) {
    if (*needle == '\0') return const_cast<char*>(haystack);
    size_t length = strlen(needle);
    for (; *haystack != '\0'; ++haystack) {
        if (strncmp(haystack, needle, length) == 0) {
            return const_cast<char*>(haystack);
        }
    }
    return nullptr;
}

extern "C" int strcasecmp(const char* lhs, const char* rhs) {
    while (*lhs != '\0' && tolower(*lhs) == tolower(*rhs)) {
        ++lhs;
        ++rhs;
    }
    return tolower(*lhs) - tolower(*rhs);
}

extern "C" int strncasecmp(const char* lhs, const char* rhs, size_t count) {
    while (count != 0 && *lhs != '\0' && tolower(*lhs) == tolower(*rhs)) {
        ++lhs;
        ++rhs;
        --count;
    }
    return count == 0 ? 0 : tolower(*lhs) - tolower(*rhs);
}

extern "C" char* strerror(int) {
    return const_cast<char*>("Neutrino error");
}

extern "C" FILE* fopen(const char* path, const char* mode) {
    if (path == nullptr || mode == nullptr) return nullptr;
    auto* stream = static_cast<NeutrinoFile*>(calloc(1, sizeof(NeutrinoFile)));
    if (stream == nullptr) return nullptr;
    stream->path = strdup(path);
    if (stream->path == nullptr) {
        free(stream);
        return nullptr;
    }
    if (mode[0] == 'r') {
        uint64_t length = 0;
        long opened = file_open(path);
        if (opened < 0 || !get_file_size(path, length) || length > SIZE_MAX) {
            if (opened >= 0) file_close(static_cast<uint32_t>(opened));
            free(stream->path);
            free(stream);
            return nullptr;
        }
        stream->kind = FileKind::Input;
        stream->size = static_cast<size_t>(length);
        stream->capacity = stream->size;
        if (stream->size != 0) {
            stream->data = static_cast<uint8_t*>(malloc(stream->size));
            if (stream->data == nullptr) {
                file_close(static_cast<uint32_t>(opened));
                free(stream->path);
                free(stream);
                return nullptr;
            }
            size_t offset = 0;
            while (offset < stream->size) {
                long read = file_read(static_cast<uint32_t>(opened),
                                      stream->data + offset,
                                      stream->size - offset);
                if (read <= 0) {
                    stream->error = true;
                    break;
                }
                offset += static_cast<size_t>(read);
            }
            stream->size = offset;
        }
        file_close(static_cast<uint32_t>(opened));
        return stream;
    }
    if (mode[0] == 'w') {
        stream->kind = FileKind::Output;
        return stream;
    }
    free(stream->path);
    free(stream);
    return nullptr;
}

extern "C" int fclose(FILE* stream) {
    if (stream == nullptr || stream == stdin || stream == stdout ||
        stream == stderr) {
        return 0;
    }
    int result = 0;
    if (stream->kind == FileKind::Output) {
        (void)file_remove(stream->path);
        long opened = file_create(stream->path);
        if (opened < 0) {
            result = -1;
        } else {
            size_t offset = 0;
            while (offset < stream->size) {
                long written = file_write(static_cast<uint32_t>(opened),
                                          stream->data + offset,
                                          stream->size - offset);
                if (written <= 0) {
                    result = -1;
                    break;
                }
                offset += static_cast<size_t>(written);
            }
            file_close(static_cast<uint32_t>(opened));
        }
    }
    free(stream->data);
    free(stream->path);
    free(stream);
    return result;
}

extern "C" size_t fread(void* ptr, size_t size, size_t count, FILE* stream) {
    if (ptr == nullptr || stream == nullptr || size == 0) return 0;
    if (count > SIZE_MAX / size) return 0;
    size_t requested = size * count;
    size_t available = stream->position < stream->size
                           ? stream->size - stream->position
                           : 0;
    size_t bytes = requested < available ? requested : available;
    memcpy(ptr, stream->data + stream->position, bytes);
    stream->position += bytes;
    return bytes / size;
}

extern "C" size_t fwrite(const void* ptr, size_t size, size_t count,
                         FILE* stream) {
    if (ptr == nullptr || stream == nullptr || size == 0) return 0;
    if (count > SIZE_MAX / size) return 0;
    size_t bytes = size * count;
    if (stream->kind == FileKind::Console) {
        uint32_t handle = console_handle(stream);
        if (handle == UINT32_MAX) return 0;
        long written = descriptor_write(handle, ptr, bytes);
        return written > 0 ? static_cast<size_t>(written) / size : 0;
    }
    if (stream->kind != FileKind::Output ||
        stream->position > SIZE_MAX - bytes ||
        !ensure_capacity(stream, stream->position + bytes)) {
        stream->error = true;
        return 0;
    }
    memcpy(stream->data + stream->position, ptr, bytes);
    stream->position += bytes;
    if (stream->position > stream->size) stream->size = stream->position;
    return count;
}

extern "C" int fseek(FILE* stream, long offset, int whence) {
    if (stream == nullptr || stream->kind == FileKind::Console) return -1;
    int64_t base = whence == SEEK_SET ? 0
                   : whence == SEEK_CUR ? static_cast<int64_t>(stream->position)
                   : whence == SEEK_END ? static_cast<int64_t>(stream->size)
                                        : -1;
    int64_t target = base + offset;
    if (base < 0 || target < 0) return -1;
    size_t position = static_cast<size_t>(target);
    if (stream->kind == FileKind::Input && position > stream->size) return -1;
    if (stream->kind == FileKind::Output && !ensure_capacity(stream, position)) {
        return -1;
    }
    stream->position = position;
    return 0;
}

extern "C" long ftell(FILE* stream) {
    return stream == nullptr ? -1 : static_cast<long>(stream->position);
}

extern "C" int feof(FILE* stream) {
    return stream != nullptr && stream->position >= stream->size;
}

extern "C" int ferror(FILE* stream) {
    return stream != nullptr && stream->error;
}

extern "C" int fflush(FILE*) {
    return 0;
}

extern "C" int vsnprintf(char* output, size_t size, const char* format,
                         va_list arguments) {
    FormatOutput out{output, size, 0};
    while (*format != '\0') {
        if (*format != '%') {
            format_char(out, *format++);
            continue;
        }
        ++format;
        if (*format == '%') {
            format_char(out, *format++);
            continue;
        }
        bool left = false;
        bool zero = false;
        bool alternate = false;
        for (;;) {
            if (*format == '-') left = true;
            else if (*format == '0') zero = true;
            else if (*format == '#') alternate = true;
            else break;
            ++format;
        }
        int width = 0;
        while (isdigit(*format)) width = width * 10 + (*format++ - '0');
        int precision = -1;
        if (*format == '.') {
            ++format;
            precision = 0;
            while (isdigit(*format)) {
                precision = precision * 10 + (*format++ - '0');
            }
        }
        enum class Length { Normal, Long, LongLong, Size };
        Length length = Length::Normal;
        if (*format == 'l') {
            ++format;
            length = Length::Long;
            if (*format == 'l') {
                ++format;
                length = Length::LongLong;
            }
        } else if (*format == 'z') {
            ++format;
            length = Length::Size;
        }
        char conversion = *format == '\0' ? '\0' : *format++;
        if (conversion == 's') {
            const char* text = va_arg(arguments, const char*);
            if (text == nullptr) text = "(null)";
            size_t text_length = strlen(text);
            if (precision >= 0 && static_cast<size_t>(precision) < text_length) {
                text_length = static_cast<size_t>(precision);
            }
            size_t padding = width > static_cast<int>(text_length)
                                 ? static_cast<size_t>(width) - text_length
                                 : 0;
            if (!left) format_repeat(out, ' ', padding);
            format_span(out, text, text_length);
            if (left) format_repeat(out, ' ', padding);
        } else if (conversion == 'c') {
            format_char(out, static_cast<char>(va_arg(arguments, int)));
        } else if (conversion == 'p') {
            auto value = reinterpret_cast<uintptr_t>(va_arg(arguments, void*));
            format_integer(out, value, false, 16, false, width,
                           precision < 0 ? 1 : precision, left, zero, true);
        } else if (conversion == 'd' || conversion == 'i') {
            int64_t value = length == Length::LongLong
                                ? va_arg(arguments, long long)
                            : length == Length::Long
                                ? va_arg(arguments, long)
                            : length == Length::Size
                                ? static_cast<int64_t>(
                                      va_arg(arguments, ptrdiff_t))
                                : va_arg(arguments, int);
            bool negative = value < 0;
            uint64_t magnitude = negative
                                     ? 0ull - static_cast<uint64_t>(value)
                                     : static_cast<uint64_t>(value);
            format_integer(out, magnitude, negative, 10, false, width,
                           precision < 0 ? 1 : precision, left, zero, false);
        } else if (conversion == 'u' || conversion == 'x' ||
                   conversion == 'X' || conversion == 'o') {
            uint64_t value = length == Length::LongLong
                                 ? va_arg(arguments, unsigned long long)
                             : length == Length::Long
                                 ? va_arg(arguments, unsigned long)
                             : length == Length::Size
                                 ? va_arg(arguments, size_t)
                                 : va_arg(arguments, unsigned int);
            unsigned base = conversion == 'o' ? 8
                            : (conversion == 'x' || conversion == 'X') ? 16
                                                                       : 10;
            format_integer(out, value, false, base, conversion == 'X', width,
                           precision < 0 ? 1 : precision, left, zero, alternate);
        } else if (conversion == 'f') {
            format_span(out, "1.000000", 8);
        } else if (conversion != '\0') {
            format_char(out, '%');
            format_char(out, conversion);
        }
    }
    if (out.capacity != 0) {
        size_t end = out.length < out.capacity ? out.length : out.capacity - 1;
        out.data[end] = '\0';
    }
    return static_cast<int>(out.length);
}

extern "C" int snprintf(char* out, size_t size, const char* format, ...) {
    va_list arguments;
    va_start(arguments, format);
    int result = vsnprintf(out, size, format, arguments);
    va_end(arguments);
    return result;
}

extern "C" int sprintf(char* out, const char* format, ...) {
    va_list arguments;
    va_start(arguments, format);
    int result = vsnprintf(out, SIZE_MAX, format, arguments);
    va_end(arguments);
    return result;
}

extern "C" int vfprintf(FILE* stream, const char* format, va_list arguments) {
    char buffer[1024];
    int length = vsnprintf(buffer, sizeof(buffer), format, arguments);
    size_t bytes = length < 0 ? 0
                   : static_cast<size_t>(length) < sizeof(buffer)
                       ? static_cast<size_t>(length)
                       : sizeof(buffer) - 1;
    return fwrite(buffer, 1, bytes, stream) == bytes ? length : -1;
}

extern "C" int fprintf(FILE* stream, const char* format, ...) {
    va_list arguments;
    va_start(arguments, format);
    int result = vfprintf(stream, format, arguments);
    va_end(arguments);
    return result;
}

extern "C" int printf(const char* format, ...) {
    char buffer[1024];
    va_list arguments;
    va_start(arguments, format);
    int length = vsnprintf(buffer, sizeof(buffer), format, arguments);
    va_end(arguments);
    size_t bytes = length < 0 ? 0
                   : static_cast<size_t>(length) < sizeof(buffer)
                       ? static_cast<size_t>(length)
                       : sizeof(buffer) - 1;
    return fwrite(buffer, 1, bytes, stdout) == bytes ? length : -1;
}

extern "C" int putchar(int ch) {
    char value = static_cast<char>(ch);
    return fwrite(&value, 1, 1, stdout) == 1 ? ch : EOF;
}

extern "C" int puts(const char* text) {
    size_t length = strlen(text);
    if (fwrite(text, 1, length, stdout) != length ||
        fwrite("\n", 1, 1, stdout) != 1) {
        return EOF;
    }
    return 0;
}

extern "C" int sscanf(const char* input, const char* format, ...) {
    while (isspace(*format)) ++format;
    while (isspace(*input)) ++input;
    va_list arguments;
    va_start(arguments, format);
    int base = 10;
    bool signed_value = true;
    if (format[0] == '0' && (format[1] == 'x' || format[1] == 'X')) {
        if (!(input[0] == '0' && (input[1] == 'x' || input[1] == 'X'))) {
            va_end(arguments);
            return 0;
        }
        input += 2;
        format += 2;
        base = 16;
        signed_value = false;
    } else if (format[0] == '0' && format[1] == '%') {
        if (*input != '0') {
            va_end(arguments);
            return 0;
        }
        ++input;
        ++format;
        base = 8;
        signed_value = false;
    }
    if (*format++ != '%') {
        va_end(arguments);
        return 0;
    }
    char conversion = *format;
    if (conversion == 'x') {
        base = 16;
        signed_value = false;
    } else if (conversion == 'o') {
        base = 8;
        signed_value = false;
    } else if (conversion != 'd' && conversion != 'i') {
        va_end(arguments);
        return 0;
    } else if (conversion == 'i') {
        base = 0;
    }
    char* end = nullptr;
    if (signed_value) {
        int* output = va_arg(arguments, int*);
        long value = strtol(input, &end, base);
        if (end != input) *output = static_cast<int>(value);
    } else {
        unsigned int* output = va_arg(arguments, unsigned int*);
        unsigned long value = strtoul(input, &end, base);
        if (end != input) *output = static_cast<unsigned int>(value);
    }
    va_end(arguments);
    return end != input ? 1 : 0;
}
