#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "descriptors.hpp"
#include "desktop_protocol.hpp"
#include "syscall.hpp"
#include "ui.hpp"

namespace neutrino::ui {

// Owns a compositor connection, client surface, and event stream. Window
// decorations deliberately are not exposed here: chrome is always rendered
// by the compositor so focus changes look identical for every application.
class Window {
public:
    Window() = default;
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;
    // Freestanding Neutrino programs do not register global destructors;
    // applications explicitly close during their normal shutdown path.
    ~Window() = default;

    bool open(uint32_t width, uint32_t height, const char* title,
              const char* surface_prefix = "neutrino.app.") {
        if (valid() || width == 0 || height == 0 || title == nullptr ||
            surface_prefix == nullptr) return false;

        long registry = shared_memory_open(desktop_protocol::kRegistryName,
                                           sizeof(desktop_protocol::Registry));
        descriptor_defs::SharedMemoryInfo registry_info{};
        if (registry < 0 ||
            shared_memory_get_info(static_cast<uint32_t>(registry),
                                   &registry_info) != 0 ||
            registry_info.base == 0 ||
            registry_info.length < sizeof(desktop_protocol::Registry)) {
            if (registry >= 0) descriptor_close(static_cast<uint32_t>(registry));
            return false;
        }
        auto published = *reinterpret_cast<const desktop_protocol::Registry*>(
            registry_info.base);
        descriptor_close(static_cast<uint32_t>(registry));
        if (published.magic != desktop_protocol::kRegistryMagic ||
            published.version != desktop_protocol::kVersion ||
            published.server_pipe_id == 0) return false;

        const uint64_t write_flags =
            static_cast<uint64_t>(descriptor_defs::Flag::Writable) |
            static_cast<uint64_t>(descriptor_defs::Flag::Async);
        const uint64_t read_flags =
            static_cast<uint64_t>(descriptor_defs::Flag::Readable) |
            static_cast<uint64_t>(descriptor_defs::Flag::Async);
        long server = pipe_open_existing(write_flags, published.server_pipe_id);
        long events = pipe_open_new(read_flags);
        descriptor_defs::PipeInfo event_info{};
        if (server < 0 || events < 0 ||
            pipe_get_info(static_cast<uint32_t>(events), &event_info) != 0 ||
            event_info.id == 0 ||
            random_get(&token_, sizeof(token_)) !=
                static_cast<long>(sizeof(token_)) || token_ == 0) {
            close_handles(server, events, -1);
            token_ = 0;
            return false;
        }

        char surface_name[48]{};
        strlcpy(surface_name, surface_prefix, sizeof(surface_name));
        append_decimal(surface_name, sizeof(surface_name), process_id());
        size_t used = strlen(surface_name);
        if (used + 1 >= sizeof(surface_name)) {
            close_handles(server, events, -1);
            token_ = 0;
            return false;
        }
        surface_name[used++] = '.';
        surface_name[used] = '\0';
        append_decimal(surface_name, sizeof(surface_name), token_);

        size_t surface_bytes = static_cast<size_t>(width) * height *
                               sizeof(uint32_t);
        long surface = shared_memory_open(surface_name, surface_bytes);
        descriptor_defs::SharedMemoryInfo surface_info{};
        if (surface < 0 ||
            shared_memory_get_info(static_cast<uint32_t>(surface),
                                   &surface_info) != 0 ||
            surface_info.base == 0 || surface_info.length < surface_bytes) {
            close_handles(server, events, surface);
            token_ = 0;
            return false;
        }

        desktop_protocol::CreateMessage request{};
        request.header = desktop_protocol::header(
            desktop_protocol::MessageType::Create, sizeof(request), 0, token_);
        request.process_id = process_id();
        request.reply_pipe_id = event_info.id;
        request.width = width;
        request.height = height;
        request.pixel_format = desktop_protocol::kPixelFormatArgb8888;
        strlcpy(request.surface_name, surface_name, sizeof(request.surface_name));
        strlcpy(request.title, title, sizeof(request.title));
        if (descriptor_write(static_cast<uint32_t>(server), &request,
                             sizeof(request)) !=
            static_cast<long>(sizeof(request))) {
            close_handles(server, events, surface);
            token_ = 0;
            return false;
        }

        desktop_protocol::CreatedMessage response{};
        size_t received = 0;
        for (uint32_t waited = 0; received < sizeof(response) && waited < 2000;
             ++waited) {
            long bytes = descriptor_read(
                static_cast<uint32_t>(events),
                reinterpret_cast<uint8_t*>(&response) + received,
                sizeof(response) - received);
            if (bytes > 0) received += static_cast<size_t>(bytes);
            else (void)sleep_ms(1);
        }
        if (received != sizeof(response) ||
            !desktop_protocol::valid(response.header,
                                     desktop_protocol::MessageType::Created,
                                     sizeof(response)) ||
            response.status != 0 || response.header.window_id == 0 ||
            response.header.token != token_) {
            close_handles(server, events, surface);
            token_ = 0;
            return false;
        }

        server_ = static_cast<uint32_t>(server);
        events_ = static_cast<uint32_t>(events);
        surface_ = static_cast<uint32_t>(surface);
        id_ = response.header.window_id;
        pixels_ = reinterpret_cast<uint32_t*>(surface_info.base);
        width_ = width;
        height_ = height;
        return true;
    }

    bool present(Rect damage) {
        if (!valid()) return false;
        damage = intersect(damage, {0, 0, static_cast<int32_t>(width_),
                                    static_cast<int32_t>(height_)});
        if (damage.width <= 0 || damage.height <= 0) return true;
        desktop_protocol::DamageMessage message{
            desktop_protocol::header(desktop_protocol::MessageType::Damage,
                                     sizeof(message), id_, token_),
            static_cast<uint32_t>(damage.x), static_cast<uint32_t>(damage.y),
            static_cast<uint32_t>(damage.width),
            static_cast<uint32_t>(damage.height)};
        return descriptor_write(server_, &message, sizeof(message)) ==
               static_cast<long>(sizeof(message));
    }

    // Returns one complete protocol message, zero if none is ready, or -1 if
    // the stream is malformed/the destination is too small.
    long next_event(void* output, size_t capacity) {
        if (!valid() || output == nullptr) return -1;
        if (event_used_ < sizeof(desktop_protocol::MessageHeader)) {
            long count = descriptor_read(events_, event_bytes_ + event_used_,
                                         sizeof(event_bytes_) - event_used_);
            if (count > 0) event_used_ += static_cast<size_t>(count);
        }
        if (event_used_ < sizeof(desktop_protocol::MessageHeader)) return 0;
        const auto* header =
            reinterpret_cast<const desktop_protocol::MessageHeader*>(event_bytes_);
        if (header->size < sizeof(*header) || header->size > sizeof(event_bytes_)) {
            event_used_ = 0;
            return -1;
        }
        if (event_used_ < header->size) {
            long count = descriptor_read(events_, event_bytes_ + event_used_,
                                         sizeof(event_bytes_) - event_used_);
            if (count > 0) event_used_ += static_cast<size_t>(count);
            if (event_used_ < header->size) return 0;
        }
        if (capacity < header->size) return -1;
        size_t message_size = header->size;
        memcpy(output, event_bytes_, message_size);
        memmove(event_bytes_, event_bytes_ + message_size,
                event_used_ - message_size);
        event_used_ -= message_size;
        return static_cast<long>(message_size);
    }

    void close() {
        if (server_ != kInvalidDescriptor && id_ != 0) {
            desktop_protocol::DestroyMessage destroy{
                desktop_protocol::header(desktop_protocol::MessageType::Destroy,
                                         sizeof(destroy), id_, token_)};
            (void)descriptor_write(server_, &destroy, sizeof(destroy));
        }
        if (surface_ != kInvalidDescriptor) descriptor_close(surface_);
        if (events_ != kInvalidDescriptor) descriptor_close(events_);
        if (server_ != kInvalidDescriptor) descriptor_close(server_);
        server_ = events_ = surface_ = kInvalidDescriptor;
        id_ = width_ = height_ = 0;
        token_ = 0;
        pixels_ = nullptr;
        event_used_ = 0;
    }

    bool valid() const { return server_ != kInvalidDescriptor && id_ != 0; }
    uint32_t* pixels() const { return pixels_; }
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    uint32_t id() const { return id_; }
    uint64_t token() const { return token_; }

private:
    static void close_handles(long server, long events, long surface) {
        if (surface >= 0) descriptor_close(static_cast<uint32_t>(surface));
        if (events >= 0) descriptor_close(static_cast<uint32_t>(events));
        if (server >= 0) descriptor_close(static_cast<uint32_t>(server));
    }
    static void append_decimal(char* output, size_t capacity, uint64_t value) {
        char reverse[24];
        size_t count = 0;
        do {
            reverse[count++] = static_cast<char>('0' + value % 10);
            value /= 10;
        } while (value != 0 && count < sizeof(reverse));
        size_t used = strlen(output);
        while (count != 0 && used + 1 < capacity)
            output[used++] = reverse[--count];
        output[used] = '\0';
    }

    uint32_t server_ = kInvalidDescriptor;
    uint32_t events_ = kInvalidDescriptor;
    uint32_t surface_ = kInvalidDescriptor;
    uint32_t id_ = 0;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    uint64_t token_ = 0;
    uint32_t* pixels_ = nullptr;
    uint8_t event_bytes_[512]{};
    size_t event_used_ = 0;
};

}  // namespace neutrino::ui
