#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "descriptors.hpp"
#include "../crt/syscall.hpp"
#include "../helpers/args.hpp"
#include "../helpers/console.hpp"

namespace {

constexpr uint32_t kDescConsole =
    static_cast<uint32_t>(descriptor_defs::Type::Console);
constexpr uint32_t kDescBlock =
    static_cast<uint32_t>(descriptor_defs::Type::BlockDevice);
constexpr long kWouldBlock = -2;

constexpr uint8_t kNeufsMagic[8] = {
    0x4E, 0x45, 0x55, 0x46, 0x53, 0x00, 0x77, 0x42};
constexpr int32_t kNeufsVersion = 2;
constexpr uint8_t kTypeNdir = 0;

#pragma pack(push, 1)
struct NeufsRvt {
    char magic[8];
    int32_t version;
    char name[16];
    uint64_t root;
    char preferred_alias[32];
};

struct NeufsNdir {
    uint8_t type;
    uint8_t reserved[7];
    char name[256];
    int64_t ctime;
    int64_t utime;
    uint64_t acl[32];
    uint64_t parent;
    uint64_t contents[64];
    uint64_t next;
    uint64_t last;
};
#pragma pack(pop)

static_assert(sizeof(NeufsRvt) % 4 == 0);
static_assert(sizeof(NeufsNdir) % 4 == 0);

struct Args {
    char device[64];
    char label[16];
    char preferred_alias[32];
    uint64_t metadata_size;
    uint32_t metadata_percent;
    bool metadata_percent_set;
    bool force;
    bool dry_run;
    bool quiet;
    bool verbose;
    bool show_help;
    bool show_version;
};

bool strings_equal(const char* a, const char* b) {
    return a != nullptr && b != nullptr && strcmp(a, b) == 0;
}

bool parse_u64(const char* text, uint64_t& out) {
    if (text == nullptr || *text == '\0') return false;
    uint64_t value = 0;
    while (*text >= '0' && *text <= '9') {
        uint64_t digit = static_cast<uint64_t>(*text - '0');
        if (value > (UINT64_MAX - digit) / 10) return false;
        value = value * 10 + digit;
        ++text;
    }
    uint64_t multiplier = 1;
    if (*text != '\0') {
        char suffix = *text++;
        if (suffix >= 'a' && suffix <= 'z') suffix -= 'a' - 'A';
        if (suffix == 'K') multiplier = 1024ull;
        else if (suffix == 'M') multiplier = 1024ull * 1024ull;
        else if (suffix == 'G') multiplier = 1024ull * 1024ull * 1024ull;
        else if (suffix == 'T') multiplier = 1024ull * 1024ull * 1024ull * 1024ull;
        else return false;
        if (*text == 'i' || *text == 'I') ++text;
        if (*text == 'b' || *text == 'B') ++text;
    }
    if (*text != '\0' || value > UINT64_MAX / multiplier) return false;
    out = value * multiplier;
    return true;
}

bool copy_option_value(const char*& cursor, char* out, size_t out_size) {
    return userspace::copy_token(cursor, out, out_size);
}

bool valid_alias(const char* alias) {
    if (alias == nullptr || alias[0] == '\0') return true;
    size_t length = 0;
    while (alias[length] != '\0') {
        char c = alias[length];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) {
            return false;
        }
        ++length;
    }
    if ((length == 1 && alias[0] == '.') ||
        (length == 2 && alias[0] == '.' && alias[1] == '.')) {
        return false;
    }
    if (length == 3) {
        char a = alias[0], b = alias[1], c = alias[2];
        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a + ('a' - 'A'));
        if (b >= 'A' && b <= 'Z') b = static_cast<char>(b + ('a' - 'A'));
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
        if (a == 's' && b == 'y' && c == 's') return false;
    }
    return true;
}

void print_progress(long console, const char* action, uint64_t done,
                    uint64_t total, bool quiet) {
    if (quiet) return;
    uint64_t percent = total == 0 ? 100 : (done * 100) / total;
    constexpr uint64_t mib = 1024ull * 1024ull;
    userspace::write(console, "mkneufs: ");
    userspace::write(console, action);
    userspace::write(console, " ");
    userspace::write_u64(console, percent);
    userspace::write(console, "% (");
    userspace::write_u64(console, done / mib);
    userspace::write(console, "/");
    userspace::write_u64(console, (total + mib - 1) / mib);
    userspace::write_line(console, " MiB)");
}

bool parse_args(const char* raw, Args& out) {
    memset(&out, 0, sizeof(out));
    out.metadata_percent = 225;  // hundredths of one percent (2.25%).
    const char* cursor = raw;
    char token[64];
    size_t positional = 0;
    bool options = true;
    while (userspace::copy_token(cursor, token, sizeof(token))) {
        if (options && strings_equal(token, "--")) {
            options = false;
        } else if (options && (strings_equal(token, "-h") ||
                               strings_equal(token, "--help"))) {
            out.show_help = true;
        } else if (options && (strings_equal(token, "-V") ||
                               strings_equal(token, "--version"))) {
            out.show_version = true;
        } else if (options && (strings_equal(token, "-f") ||
                               strings_equal(token, "--force"))) {
            out.force = true;
        } else if (options && (strings_equal(token, "-n") ||
                               strings_equal(token, "--dry-run"))) {
            out.dry_run = true;
        } else if (options && (strings_equal(token, "-q") ||
                               strings_equal(token, "--quiet"))) {
            out.quiet = true;
        } else if (options && (strings_equal(token, "-v") ||
                               strings_equal(token, "--verbose"))) {
            out.verbose = true;
        } else if (options && (strings_equal(token, "-L") ||
                               strings_equal(token, "--label"))) {
            if (!copy_option_value(cursor, out.label, sizeof(out.label))) return false;
        } else if (options && (strings_equal(token, "-A") ||
                               strings_equal(token, "--alias"))) {
            if (!copy_option_value(cursor, out.preferred_alias,
                                   sizeof(out.preferred_alias))) return false;
        } else if (options && (strings_equal(token, "-m") ||
                               strings_equal(token, "--metadata-size"))) {
            char value[32];
            if (!copy_option_value(cursor, value, sizeof(value)) ||
                !parse_u64(value, out.metadata_size) || out.metadata_size == 0) {
                return false;
            }
        } else if (options && strings_equal(token, "--metadata-percent")) {
            char value[16];
            uint64_t parsed = 0;
            if (!copy_option_value(cursor, value, sizeof(value)) ||
                !parse_u64(value, parsed) || parsed == 0 || parsed > 99) return false;
            out.metadata_percent = static_cast<uint32_t>(parsed * 100);
            out.metadata_percent_set = true;
        } else if (options && token[0] == '-') {
            return false;
        } else if (out.device[0] == '\0') {
            memcpy(out.device, token, strlen(token) + 1);
        } else if (positional == 0 && out.label[0] == '\0') {
            memcpy(out.label, token, strlen(token) + 1);
            ++positional;
        } else if (positional <= 1 && out.preferred_alias[0] == '\0') {
            memcpy(out.preferred_alias, token, strlen(token) + 1);
            ++positional;
        } else {
            return false;
        }
    }
    if (out.show_help || out.show_version) return true;
    if (out.device[0] == '\0') return false;
    if (out.label[0] == '\0') memcpy(out.label, "neufs", 6);
    return valid_alias(out.preferred_alias);
}

uint64_t align_up(uint64_t value, uint64_t alignment) {
    if (alignment == 0) {
        return value;
    }
    uint64_t rem = value % alignment;
    return rem == 0 ? value : value + (alignment - rem);
}

uint64_t default_meta_size(uint64_t total_bytes, uint64_t sector_size,
                           uint32_t hundredths_percent) {
    uint64_t suggested = (total_bytes / 10000) * hundredths_percent;
    const uint64_t min_meta = 256ull * 1024ull * 1024ull;
    const uint64_t max_meta = 16ull * 1024ull * 1024ull * 1024ull;
    if (suggested < min_meta && total_bytes >= min_meta * 2) {
        suggested = min_meta;
    }
    if (suggested > max_meta) {
        suggested = max_meta;
    }
    if (sector_size != 0 && suggested > total_bytes - sector_size) {
        suggested = total_bytes - sector_size;
    } else if (sector_size == 0 && suggested > total_bytes) {
        suggested = total_bytes;
    }
    return align_up(suggested, sector_size);
}

long read_exact(uint32_t handle, void* buffer, size_t length, uint64_t offset);

void print_help(long console) {
    userspace::write_line(console, "usage: mkneufs [options] <block-device> [label [alias]]");
    userspace::write_line(console, "Create a NEUFS v2 filesystem.");
    userspace::write_line(console, "  -L, --label NAME          set volume label (max 15 characters)");
    userspace::write_line(console, "  -A, --alias NAME          set preferred @ namespace alias");
    userspace::write_line(console, "  -m, --metadata-size SIZE  set metadata area (K/M/G/T suffixes allowed)");
    userspace::write_line(console, "      --metadata-percent N  use N percent of the device for metadata");
    userspace::write_line(console, "  -n, --dry-run             validate and print layout without writing");
    userspace::write_line(console, "  -f, --force               overwrite a recognized existing filesystem");
    userspace::write_line(console, "  -q, --quiet               suppress normal output");
    userspace::write_line(console, "  -v, --verbose             print additional layout information");
    userspace::write_line(console, "  -h, --help                display this help");
    userspace::write_line(console, "  -V, --version             display version");
}

bool has_filesystem_signature(uint32_t handle, uint64_t sector_size) {
    if (sector_size < 512 || sector_size > 4096) return false;
    uint8_t sector[4096];
    if (read_exact(handle, sector, static_cast<size_t>(sector_size), 0) !=
        static_cast<long>(sector_size)) return false;
    if (memcmp(sector, kNeufsMagic, sizeof(kNeufsMagic)) == 0) return true;
    if (memcmp(sector + 82, "FAT32   ", 8) == 0 ||
        memcmp(sector + 54, "FAT16   ", 8) == 0 ||
        memcmp(sector + 54, "FAT12   ", 8) == 0) return true;
    return false;
}

long read_exact(uint32_t handle, void* buffer, size_t length, uint64_t offset) {
    while (true) {
        long result = descriptor_read(handle, buffer, length, offset);
        if (result == kWouldBlock) {
            yield();
            continue;
        }
        return result;
    }
}

long write_exact(uint32_t handle,
                 const void* buffer,
                 size_t length,
                 uint64_t offset) {
    while (true) {
        long result = descriptor_write(handle, buffer, length, offset);
        if (result == kWouldBlock) {
            yield();
            continue;
        }
        return result;
    }
}

bool zero_region(long console,
                 uint32_t handle,
                 uint64_t offset,
                 uint64_t size,
                 uint8_t* scratch,
                 uint64_t scratch_size,
                 bool quiet) {
    uint64_t total_size = size;
    uint64_t done = 0;
    constexpr uint64_t progress_step = 1024ull * 1024ull;
    uint64_t next_progress = progress_step;

    memset(scratch, 0, static_cast<size_t>(scratch_size));
    print_progress(console, "clearing metadata", 0, total_size, quiet);
    while (size > 0) {
        uint64_t chunk = size < scratch_size ? size : scratch_size;
        if (write_exact(handle,
                        scratch,
                        static_cast<size_t>(chunk),
                        offset) != static_cast<long>(chunk)) {
            return false;
        }
        offset += chunk;
        size -= chunk;
        done += chunk;

        if (done >= next_progress || size == 0) {
            print_progress(console, "clearing metadata", done, total_size, quiet);
            while (next_progress <= done) {
                next_progress += progress_step;
            }
        }
    }
    return true;
}

bool write_bytes(uint32_t handle,
                 uint64_t offset,
                 const void* data,
                 size_t size,
                 uint8_t* sector,
                 uint64_t sector_size) {
    const uint8_t* src = static_cast<const uint8_t*>(data);
    while (size > 0) {
        uint64_t sector_offset = (offset / sector_size) * sector_size;
        size_t in_sector = static_cast<size_t>(offset - sector_offset);
        size_t chunk = static_cast<size_t>(sector_size) - in_sector;
        if (chunk > size) {
            chunk = size;
        }

        if (read_exact(handle,
                       sector,
                       static_cast<size_t>(sector_size),
                       sector_offset) != static_cast<long>(sector_size)) {
            return false;
        }
        memcpy(sector + in_sector, src, chunk);
        if (write_exact(handle,
                        sector,
                        static_cast<size_t>(sector_size),
                        sector_offset) != static_cast<long>(sector_size)) {
            return false;
        }

        offset += chunk;
        src += chunk;
        size -= chunk;
    }
    return true;
}

bool bitmap_set_range(uint8_t* sector,
                      uint32_t handle,
                      uint64_t bitmap_offset,
                      uint64_t start_bit,
                      uint64_t count,
                      uint64_t sector_size) {
    uint64_t end_bit = start_bit + count;
    uint64_t current_bit = start_bit;

    while (current_bit < end_bit) {
        uint64_t byte_offset = bitmap_offset + (current_bit / 8);
        uint64_t sector_offset = (byte_offset / sector_size) * sector_size;
        if (read_exact(handle,
                       sector,
                       static_cast<size_t>(sector_size),
                       sector_offset) != static_cast<long>(sector_size)) {
            return false;
        }

        while (current_bit < end_bit) {
            uint64_t current_byte = bitmap_offset + (current_bit / 8);
            if ((current_byte / sector_size) * sector_size != sector_offset) {
                break;
            }
            size_t in_sector = static_cast<size_t>(current_byte - sector_offset);
            uint8_t bit_in_byte = static_cast<uint8_t>(current_bit % 8);
            uint8_t mask = 0;
            while (bit_in_byte < 8 && current_bit < end_bit) {
                mask |= static_cast<uint8_t>(1u << bit_in_byte);
                ++bit_in_byte;
                ++current_bit;
            }
            sector[in_sector] |= mask;
        }

        if (write_exact(handle,
                        sector,
                        static_cast<size_t>(sector_size),
                        sector_offset) != static_cast<long>(sector_size)) {
            return false;
        }
    }
    return true;
}

bool format_neufs(long console,
                  uint32_t handle,
                  const Args& args,
                  const descriptor_defs::BlockGeometry& geom) {
    if (geom.sector_size == 0 || geom.sector_count == 0 ||
        geom.sector_size > 4096) {
        userspace::write_line(console, "mkneufs: unsupported block geometry");
        return false;
    }

    if (geom.sector_count > UINT64_MAX / geom.sector_size) {
        userspace::write_line(console, "mkneufs: block geometry overflows address space");
        return false;
    }
    uint64_t total_bytes = geom.sector_size * geom.sector_count;
    if (args.metadata_size != 0 &&
        args.metadata_size > total_bytes - (geom.sector_size - 1)) {
        userspace::write_line(console, "mkneufs: requested metadata area is too large");
        return false;
    }
    uint64_t meta_size = args.metadata_size != 0
                             ? align_up(args.metadata_size, geom.sector_size)
                             : args.metadata_percent_set
                                   ? align_up((total_bytes / 100) *
                                                  (args.metadata_percent / 100),
                                              geom.sector_size)
                                   : default_meta_size(total_bytes,
                                                       geom.sector_size,
                                                       args.metadata_percent);
    if (meta_size >= total_bytes) {
        userspace::write_line(console, "mkneufs: device too small for spec default metadata area");
        return false;
    }

    uint64_t data_bitmap_offset = align_up(sizeof(NeufsRvt), 8);
    uint64_t data_bitmap_size = (geom.sector_count + 7) / 8;
    uint64_t meta_blocks = meta_size / 8;
    uint64_t meta_bitmap_offset =
        align_up(data_bitmap_offset + data_bitmap_size, 8);
    uint64_t meta_bitmap_size = (meta_blocks + 7) / 8;
    uint64_t bitmap_end = meta_bitmap_offset + meta_bitmap_size;
    uint64_t root_offset = align_up(bitmap_end, geom.sector_size);
    if (root_offset + sizeof(NeufsNdir) > meta_size) {
        userspace::write_line(console, "mkneufs: metadata area cannot fit root directory");
        return false;
    }

    if (args.dry_run) {
        userspace::write(console, "mkneufs: would format ");
        userspace::write(console, args.device);
        userspace::write(console, " as NEUFS v2; sectors=");
        userspace::write_u64(console, geom.sector_count);
        userspace::write(console, " sector_size=");
        userspace::write_u64(console, geom.sector_size);
        userspace::write(console, " metadata_bytes=");
        userspace::write_u64(console, meta_size);
        userspace::write_line(console, "");
        return true;
    }

    uint64_t sectors_per_write = 255;
    uint64_t scratch_size = geom.sector_size * sectors_per_write;
    uint8_t* sector = static_cast<uint8_t*>(
        map_anonymous(static_cast<size_t>(scratch_size), MAP_WRITE));
    if (sector == nullptr) {
        userspace::write_line(console, "mkneufs: allocation failed");
        return false;
    }

    if (!zero_region(console, handle, 0, meta_size, sector, scratch_size,
                     args.quiet)) {
        userspace::write_line(console, "mkneufs: failed to clear metadata area");
        unmap(sector, static_cast<size_t>(scratch_size));
        return false;
    }

    if (!args.quiet) userspace::write_line(console, "mkneufs: writing RVT");
    NeufsRvt rvt{};
    for (size_t i = 0; i < sizeof(kNeufsMagic); ++i) {
        rvt.magic[i] = static_cast<char>(kNeufsMagic[i]);
    }
    rvt.version = kNeufsVersion;
    memcpy(rvt.name, args.label, strlen(args.label));
    memcpy(rvt.preferred_alias, args.preferred_alias,
           strlen(args.preferred_alias));
    rvt.root = root_offset;
    if (!write_bytes(handle, 0, &rvt, sizeof(rvt), sector, geom.sector_size)) {
        userspace::write_line(console, "mkneufs: failed to write RVT");
        unmap(sector, static_cast<size_t>(scratch_size));
        return false;
    }

    if (!args.quiet) userspace::write_line(console, "mkneufs: writing root directory");
    NeufsNdir root{};
    root.type = kTypeNdir;
    root.name[0] = '/';
    root.name[1] = '\0';
    if (!write_bytes(handle,
                     root_offset,
                     &root,
                     sizeof(root),
                     sector,
                     geom.sector_size)) {
        userspace::write_line(console, "mkneufs: failed to write root directory");
        unmap(sector, static_cast<size_t>(scratch_size));
        return false;
    }

    uint64_t used_data_sectors = align_up(meta_size, geom.sector_size) /
                                 geom.sector_size;
    if (!args.quiet) userspace::write_line(console, "mkneufs: initializing data bitmap");
    if (!bitmap_set_range(sector,
                          handle,
                          data_bitmap_offset,
                          0,
                          used_data_sectors,
                          geom.sector_size)) {
        userspace::write_line(console, "mkneufs: failed to initialize data bitmap");
        unmap(sector, static_cast<size_t>(scratch_size));
        return false;
    }

    uint64_t rvt_blocks = align_up(sizeof(NeufsRvt), 8) / 8;
    uint64_t data_bitmap_blocks = align_up(data_bitmap_size, 8) / 8;
    uint64_t meta_bitmap_blocks = align_up(meta_bitmap_size, 8) / 8;
    uint64_t root_blocks = align_up(sizeof(NeufsNdir), 8) / 8;
    if (!args.quiet) userspace::write_line(console, "mkneufs: initializing metadata bitmap");
    if (!bitmap_set_range(sector, handle, meta_bitmap_offset, 0,
                          rvt_blocks, geom.sector_size) ||
        !bitmap_set_range(sector, handle, meta_bitmap_offset,
                          data_bitmap_offset / 8,
                          data_bitmap_blocks,
                          geom.sector_size) ||
        !bitmap_set_range(sector, handle, meta_bitmap_offset,
                          meta_bitmap_offset / 8,
                          meta_bitmap_blocks,
                          geom.sector_size) ||
        !bitmap_set_range(sector, handle, meta_bitmap_offset,
                          root_offset / 8,
                          root_blocks,
                          geom.sector_size)) {
        userspace::write_line(console, "mkneufs: failed to initialize metadata bitmap");
        unmap(sector, static_cast<size_t>(scratch_size));
        return false;
    }

    unmap(sector, static_cast<size_t>(scratch_size));

    if (args.quiet) return true;
    userspace::write(console, "mkneufs: formatted ");
    userspace::write(console, args.device);
    userspace::write(console, " label=");
    userspace::write_line(console, args.label);
    if (args.preferred_alias[0] != '\0') {
        userspace::write(console, "mkneufs: preferred_alias=@");
        userspace::write_line(console, args.preferred_alias);
    }
    userspace::write(console, "mkneufs: sectors=");
    userspace::write_u64(console, geom.sector_count);
    userspace::write(console, " sector_size=");
    userspace::write_u64(console, geom.sector_size);
    userspace::write(console, " meta_bytes=");
    userspace::write_u64(console, meta_size);
    userspace::write(console, " root=");
    userspace::write_u64(console, root_offset);
    userspace::write_line(console, "");
    if (args.verbose) {
        userspace::write(console, "mkneufs: data_bitmap=");
        userspace::write_u64(console, data_bitmap_offset);
        userspace::write(console, " metadata_bitmap=");
        userspace::write_u64(console, meta_bitmap_offset);
        userspace::write_line(console, "");
    }
    return true;
}

}  // namespace

int main(uint64_t arg_ptr, uint64_t) {
    long console = process_get_standard_descriptor(1);
    if (console < 0) {
        console = descriptor_open(kDescConsole, 0);
    }

    Args args{};
    const char* raw = reinterpret_cast<const char*>(arg_ptr);
    if (!parse_args(raw, args)) {
        userspace::write_line(console, "mkneufs: invalid arguments");
        userspace::write_line(console, "Try 'mkneufs --help' for more information.");
        return 1;
    }
    if (args.show_help) {
        print_help(console);
        return 0;
    }
    if (args.show_version) {
        userspace::write_line(console, "mkneufs (system-tools) 1.4.0");
        return 0;
    }

    long device = descriptor_open(
        kDescBlock,
        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(args.device)),
        0,
        0);

    if (device < 0) {
        userspace::write(console, "mkneufs: unable to open ");
        userspace::write_line(console, args.device);
        return 1;
    }

    if (!args.dry_run &&
        descriptor_test_flag(static_cast<uint32_t>(device),
                             static_cast<uint64_t>(
                                 descriptor_defs::Flag::Writable)) != 1) {
        userspace::write_line(console, "mkneufs: block device is not writable");
        descriptor_close(static_cast<uint32_t>(device));
        return 1;
    }

    descriptor_defs::BlockGeometry geom{};
    if (descriptor_get_property(
            static_cast<uint32_t>(device),
            static_cast<uint32_t>(descriptor_defs::Property::BlockGeometry),
            &geom,
            sizeof(geom)) != 0) {
        userspace::write_line(console, "mkneufs: failed to query block geometry");
        descriptor_close(static_cast<uint32_t>(device));
        return 1;
    }

    if (!args.force &&
        has_filesystem_signature(static_cast<uint32_t>(device),
                                 geom.sector_size)) {
        userspace::write_line(console,
                              "mkneufs: existing filesystem signature found; use --force to overwrite");
        descriptor_close(static_cast<uint32_t>(device));
        return 1;
    }

    bool ok = format_neufs(console,
                           static_cast<uint32_t>(device),
                           args,
                           geom);
    descriptor_close(static_cast<uint32_t>(device));
    return ok ? 0 : 1;
}
