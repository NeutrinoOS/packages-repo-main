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
constexpr uint32_t kSectorSize = 512;
constexpr uint32_t kFat32MinimumClusters = 65525;

struct Args {
    char device[64];
    char label[12];
    uint32_t volume_id;
    uint32_t sectors_per_cluster;
    uint32_t reserved_sectors;
    uint32_t num_fats;
    uint32_t hidden_sectors;
    uint64_t sector_limit;
    bool has_label;
    bool has_volume_id;
    bool has_hidden_sectors;
    bool invariant;
    bool force;
    bool dry_run;
    bool quiet;
    bool verbose;
    bool show_help;
    bool show_version;
};

struct Layout {
    uint32_t total_sectors;
    uint32_t reserved_sectors;
    uint32_t sectors_per_cluster;
    uint32_t num_fats;
    uint32_t fat_sectors;
    uint32_t data_start;
    uint32_t clusters;
    uint32_t hidden_sectors;
};

bool equal(const char* a, const char* b) {
    return a != nullptr && b != nullptr && strcmp(a, b) == 0;
}

uint8_t hex_digit(char ch) {
    if (ch >= '0' && ch <= '9') return static_cast<uint8_t>(ch - '0');
    if (ch >= 'a' && ch <= 'f') return static_cast<uint8_t>(ch - 'a' + 10);
    if (ch >= 'A' && ch <= 'F') return static_cast<uint8_t>(ch - 'A' + 10);
    return 0xff;
}

bool parse_u64(const char* text, uint64_t& out) {
    if (text == nullptr || *text == '\0') return false;
    uint32_t base = 10;
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        base = 16;
        text += 2;
    }
    if (*text == '\0') return false;
    uint64_t value = 0;
    while (*text != '\0') {
        uint8_t digit = base == 16 ? hex_digit(*text)
                                   : (*text >= '0' && *text <= '9'
                                          ? static_cast<uint8_t>(*text - '0')
                                          : 0xff);
        if (digit >= base || value > (UINT64_MAX - digit) / base) return false;
        value = value * base + digit;
        ++text;
    }
    out = value;
    return true;
}

bool parse_value(const char*& cursor, uint64_t& out) {
    char token[32];
    return userspace::copy_token(cursor, token, sizeof(token)) &&
           parse_u64(token, out);
}

bool valid_label(const char* label) {
    size_t length = strlen(label);
    if (length == 0 || length > 11) return false;
    for (size_t i = 0; i < length; ++i) {
        unsigned char ch = static_cast<unsigned char>(label[i]);
        if (ch < 0x20 || ch > 0x7e || ch == '"' || ch == '*' || ch == '+' ||
            ch == ',' || ch == '.' || ch == '/' || ch == ':' || ch == ';' ||
            ch == '<' || ch == '=' || ch == '>' || ch == '?' || ch == '[' ||
            ch == '\\' || ch == ']' || ch == '|') return false;
    }
    return true;
}

void make_label(const char* input, char (&output)[12]) {
    memset(output, ' ', 11);
    output[11] = '\0';
    for (size_t i = 0; input[i] != '\0' && i < 11; ++i) {
        char ch = input[i];
        if (ch >= 'a' && ch <= 'z') ch -= 'a' - 'A';
        output[i] = ch;
    }
}

bool parse_args(const char* raw, Args& out) {
    out = {};
    out.reserved_sectors = 32;
    out.num_fats = 2;
    const char* cursor = raw;
    char token[64];
    bool options = true;
    while (userspace::copy_token(cursor, token, sizeof(token))) {
        if (options && equal(token, "--")) {
            options = false;
        } else if (options && (equal(token, "-h") || equal(token, "--help"))) {
            out.show_help = true;
        } else if (options && (equal(token, "-V") || equal(token, "--version"))) {
            out.show_version = true;
        } else if (options && (equal(token, "-I") || equal(token, "--force"))) {
            out.force = true;
        } else if (options && equal(token, "--dry-run")) {
            out.dry_run = true;
        } else if (options && (equal(token, "-q") || equal(token, "--quiet"))) {
            out.quiet = true;
        } else if (options && (equal(token, "-v") || equal(token, "--verbose"))) {
            out.verbose = true;
        } else if (options && equal(token, "--invariant")) {
            out.invariant = true;
        } else if (options && (equal(token, "-n") || equal(token, "--name") ||
                               equal(token, "-L") || equal(token, "--label"))) {
            char value[12];
            if (!userspace::copy_token(cursor, value, sizeof(value)) ||
                !valid_label(value)) return false;
            make_label(value, out.label);
            out.has_label = true;
        } else if (options && (equal(token, "-i") || equal(token, "--volume-id"))) {
            uint64_t value = 0;
            if (!parse_value(cursor, value) || value > UINT32_MAX) return false;
            out.volume_id = static_cast<uint32_t>(value);
            out.has_volume_id = true;
        } else if (options && (equal(token, "-s") ||
                               equal(token, "--sectors-per-cluster"))) {
            uint64_t value = 0;
            if (!parse_value(cursor, value) || value == 0 || value > 64 ||
                (value & (value - 1)) != 0) return false;
            out.sectors_per_cluster = static_cast<uint32_t>(value);
        } else if (options && (equal(token, "-R") ||
                               equal(token, "--reserved-sectors"))) {
            uint64_t value = 0;
            if (!parse_value(cursor, value) || value < 8 || value > UINT16_MAX)
                return false;
            out.reserved_sectors = static_cast<uint32_t>(value);
        } else if (options && (equal(token, "-f") || equal(token, "--fat-count"))) {
            uint64_t value = 0;
            if (!parse_value(cursor, value) || value == 0 || value > 4) return false;
            out.num_fats = static_cast<uint32_t>(value);
        } else if (options && (equal(token, "-S") || equal(token, "--sector-size"))) {
            uint64_t value = 0;
            if (!parse_value(cursor, value) || value != kSectorSize) return false;
        } else if (options && (equal(token, "--sectors") || equal(token, "--size"))) {
            if (!parse_value(cursor, out.sector_limit) || out.sector_limit == 0)
                return false;
        } else if (options && equal(token, "--hidden-sectors")) {
            uint64_t value = 0;
            if (!parse_value(cursor, value) || value > UINT32_MAX) return false;
            out.hidden_sectors = static_cast<uint32_t>(value);
            out.has_hidden_sectors = true;
        } else if (options && equal(token, "-F")) {
            uint64_t value = 0;
            if (!parse_value(cursor, value) || value != 32) return false;
        } else if (options && token[0] == '-') {
            return false;
        } else if (out.device[0] == '\0') {
            memcpy(out.device, token, strlen(token) + 1);
        } else {
            return false;
        }
    }
    return out.show_help || out.show_version || out.device[0] != '\0';
}

void print_help(long console) {
    userspace::write_line(console, "usage: mkfat [options] <block-device>");
    userspace::write_line(console, "Create a FAT32 filesystem (mkfs.fat-compatible options where applicable).");
    userspace::write_line(console, "  -F 32                       select FAT32 (only supported format)");
    userspace::write_line(console, "  -n, -L, --label NAME         set volume label (max 11 characters)");
    userspace::write_line(console, "  -i, --volume-id ID           set 32-bit volume ID (decimal or 0xhex)");
    userspace::write_line(console, "  -s, --sectors-per-cluster N  set power-of-two cluster size (1..64)");
    userspace::write_line(console, "  -R, --reserved-sectors N     set reserved sector count (8..65535)");
    userspace::write_line(console, "  -f, --fat-count N            set number of FATs (1..4)");
    userspace::write_line(console, "  -S, --sector-size 512         validate logical sector size");
    userspace::write_line(console, "      --sectors N               use only the first N sectors");
    userspace::write_line(console, "      --hidden-sectors N        set partition offset recorded in the BPB");
    userspace::write_line(console, "      --invariant               use a reproducible default volume ID");
    userspace::write_line(console, "      --dry-run                 validate and print layout without writing");
    userspace::write_line(console, "  -I, --force                  overwrite a recognized filesystem");
    userspace::write_line(console, "  -q, --quiet                  suppress normal output");
    userspace::write_line(console, "  -v, --verbose                print detailed layout");
    userspace::write_line(console, "  -h, --help                   display this help");
    userspace::write_line(console, "  -V, --version                display version");
}

long read_exact(uint32_t handle, void* buffer, size_t length, uint64_t offset) {
    while (true) {
        long result = descriptor_read(handle, buffer, length, offset);
        if (result == kWouldBlock) { yield(); continue; }
        return result;
    }
}

long write_exact(uint32_t handle, const void* buffer, size_t length,
                 uint64_t offset) {
    while (true) {
        long result = descriptor_write(handle, buffer, length, offset);
        if (result == kWouldBlock) { yield(); continue; }
        return result;
    }
}

void put_u16(uint8_t* out, uint16_t value) {
    out[0] = static_cast<uint8_t>(value);
    out[1] = static_cast<uint8_t>(value >> 8);
}

void put_u32(uint8_t* out, uint32_t value) {
    out[0] = static_cast<uint8_t>(value);
    out[1] = static_cast<uint8_t>(value >> 8);
    out[2] = static_cast<uint8_t>(value >> 16);
    out[3] = static_cast<uint8_t>(value >> 24);
}

bool existing_filesystem(uint32_t handle) {
    uint8_t sector[kSectorSize];
    if (read_exact(handle, sector, sizeof(sector), 0) != sizeof(sector)) return false;
    static constexpr uint8_t neufs_magic[8] =
        {0x4e, 0x45, 0x55, 0x46, 0x53, 0x00, 0x77, 0x42};
    return memcmp(sector, neufs_magic, 8) == 0 ||
           memcmp(sector + 82, "FAT32   ", 8) == 0 ||
           memcmp(sector + 54, "FAT16   ", 8) == 0 ||
           memcmp(sector + 54, "FAT12   ", 8) == 0;
}

uint32_t automatic_cluster_size(uint64_t bytes) {
    if (bytes <= 260ull * 1024 * 1024) return 1;
    if (bytes <= 8ull * 1024 * 1024 * 1024) return 8;
    if (bytes <= 16ull * 1024 * 1024 * 1024) return 16;
    if (bytes <= 32ull * 1024 * 1024 * 1024) return 32;
    return 64;
}

bool calculate_layout(const Args& args,
                      const descriptor_defs::BlockGeometry& geometry,
                      Layout& out) {
    if (geometry.sector_size != kSectorSize || geometry.sector_count == 0 ||
        geometry.sector_count > UINT32_MAX) return false;
    uint64_t sectors = args.sector_limit != 0 ? args.sector_limit
                                               : geometry.sector_count;
    if (sectors > geometry.sector_count || sectors > UINT32_MAX) return false;
    out = {};
    out.total_sectors = static_cast<uint32_t>(sectors);
    out.reserved_sectors = args.reserved_sectors;
    out.num_fats = args.num_fats;
    out.hidden_sectors = args.hidden_sectors;
    out.sectors_per_cluster = args.sectors_per_cluster != 0
                                  ? args.sectors_per_cluster
                                  : automatic_cluster_size(sectors * kSectorSize);
    uint64_t maximum_clusters =
        static_cast<uint64_t>(out.total_sectors) / out.sectors_per_cluster;
    uint32_t fat_sectors = static_cast<uint32_t>(
        ((maximum_clusters + 2) * 4 + kSectorSize - 1) / kSectorSize);
    bool converged = false;
    for (unsigned pass = 0; pass < 16; ++pass) {
        uint64_t overhead = static_cast<uint64_t>(out.reserved_sectors) +
                            static_cast<uint64_t>(out.num_fats) * fat_sectors;
        if (overhead >= out.total_sectors) return false;
        uint32_t clusters = static_cast<uint32_t>(
            (out.total_sectors - overhead) / out.sectors_per_cluster);
        uint32_t needed = static_cast<uint32_t>(
            (static_cast<uint64_t>(clusters + 2) * 4 + kSectorSize - 1) /
            kSectorSize);
        if (needed == fat_sectors) {
            converged = true;
            break;
        }
        if (needed > fat_sectors) {
            // Rounding can produce a two-value cycle. The larger value is the
            // smallest one with room for every resulting cluster entry.
            fat_sectors = needed;
            converged = true;
            break;
        }
        fat_sectors = needed;
    }
    if (!converged) return false;
    out.fat_sectors = fat_sectors;
    out.data_start = out.reserved_sectors + out.num_fats * out.fat_sectors;
    if (out.data_start >= out.total_sectors) return false;
    out.clusters = (out.total_sectors - out.data_start) /
                   out.sectors_per_cluster;
    uint64_t fat_entries = static_cast<uint64_t>(out.fat_sectors) *
                           kSectorSize / 4;
    return out.clusters >= kFat32MinimumClusters &&
           out.clusters <= 0x0ffffff5u && fat_entries >= out.clusters + 2ull;
}

uint32_t default_volume_id(const Args& args, const Layout& layout) {
    if (args.invariant) return 0x4e455546u;
    uint32_t random_id = 0;
    if (random_get(&random_id, sizeof(random_id)) ==
        static_cast<long>(sizeof(random_id))) return random_id;
    uint32_t hash = 2166136261u;
    for (size_t i = 0; args.device[i] != '\0'; ++i) {
        hash ^= static_cast<uint8_t>(args.device[i]);
        hash *= 16777619u;
    }
    hash ^= layout.total_sectors;
    hash *= 16777619u;
    return hash;
}

bool zero_sectors(uint32_t handle, uint32_t first, uint32_t count,
                  uint8_t* scratch, uint32_t scratch_sectors) {
    memset(scratch, 0, static_cast<size_t>(scratch_sectors) * kSectorSize);
    while (count != 0) {
        uint32_t chunk = count < scratch_sectors ? count : scratch_sectors;
        size_t bytes = static_cast<size_t>(chunk) * kSectorSize;
        if (write_exact(handle, scratch, bytes,
                        static_cast<uint64_t>(first) * kSectorSize) !=
            static_cast<long>(bytes)) return false;
        first += chunk;
        count -= chunk;
    }
    return true;
}

void build_boot_sector(uint8_t* sector, const Args& args,
                       const Layout& layout, uint32_t volume_id) {
    memset(sector, 0, kSectorSize);
    sector[0] = 0xeb; sector[1] = 0x58; sector[2] = 0x90;
    memcpy(sector + 3, "NEUTRINO", 8);
    put_u16(sector + 11, kSectorSize);
    sector[13] = static_cast<uint8_t>(layout.sectors_per_cluster);
    put_u16(sector + 14, static_cast<uint16_t>(layout.reserved_sectors));
    sector[16] = static_cast<uint8_t>(layout.num_fats);
    put_u16(sector + 17, 0);
    put_u16(sector + 19, 0);
    sector[21] = 0xf8;
    put_u16(sector + 22, 0);
    put_u16(sector + 24, 63);
    put_u16(sector + 26, 255);
    put_u32(sector + 28, layout.hidden_sectors);
    put_u32(sector + 32, layout.total_sectors);
    put_u32(sector + 36, layout.fat_sectors);
    put_u16(sector + 40, 0);
    put_u16(sector + 42, 0);
    put_u32(sector + 44, 2);
    put_u16(sector + 48, 1);
    put_u16(sector + 50, 6);
    sector[64] = 0x80;
    sector[66] = 0x29;
    put_u32(sector + 67, volume_id);
    memcpy(sector + 71, args.has_label ? args.label : "NO NAME    ", 11);
    memcpy(sector + 82, "FAT32   ", 8);
    sector[510] = 0x55;
    sector[511] = 0xaa;
}

void build_fsinfo(uint8_t* sector, const Layout& layout) {
    memset(sector, 0, kSectorSize);
    put_u32(sector + 0, 0x41615252u);
    put_u32(sector + 484, 0x61417272u);
    put_u32(sector + 488, layout.clusters - 1);
    put_u32(sector + 492, 3);
    put_u32(sector + 508, 0xaa550000u);
}

bool write_filesystem(uint32_t handle, const Args& args, const Layout& layout,
                      uint32_t volume_id) {
    constexpr uint32_t scratch_sectors = 255;
    uint8_t* scratch = static_cast<uint8_t*>(
        map_anonymous(scratch_sectors * kSectorSize, MAP_WRITE));
    if (scratch == nullptr) return false;
    uint32_t metadata_sectors = layout.data_start + layout.sectors_per_cluster;
    bool ok = zero_sectors(handle, 0, metadata_sectors, scratch, scratch_sectors);
    if (ok) {
        build_boot_sector(scratch, args, layout, volume_id);
        ok = write_exact(handle, scratch, kSectorSize, 0) == kSectorSize &&
             write_exact(handle, scratch, kSectorSize,
                         static_cast<uint64_t>(6) * kSectorSize) == kSectorSize;
    }
    if (ok) {
        build_fsinfo(scratch, layout);
        ok = write_exact(handle, scratch, kSectorSize,
                         static_cast<uint64_t>(1) * kSectorSize) == kSectorSize &&
             write_exact(handle, scratch, kSectorSize,
                         static_cast<uint64_t>(7) * kSectorSize) == kSectorSize;
    }
    if (ok) {
        memset(scratch, 0, kSectorSize);
        put_u32(scratch + 0, 0x0ffffff8u);
        put_u32(scratch + 4, 0xffffffffu);
        put_u32(scratch + 8, 0x0fffffffu);
        for (uint32_t fat = 0; fat < layout.num_fats && ok; ++fat) {
            uint32_t sector = layout.reserved_sectors + fat * layout.fat_sectors;
            ok = write_exact(handle, scratch, kSectorSize,
                             static_cast<uint64_t>(sector) * kSectorSize) ==
                 kSectorSize;
        }
    }
    if (ok && args.has_label) {
        memset(scratch, 0, kSectorSize);
        memcpy(scratch, args.label, 11);
        scratch[11] = 0x08;
        ok = write_exact(handle, scratch, kSectorSize,
                         static_cast<uint64_t>(layout.data_start) * kSectorSize) ==
             kSectorSize;
    }
    unmap(scratch, scratch_sectors * kSectorSize);
    return ok;
}

void print_layout(long console, const Args& args, const Layout& layout,
                  uint32_t volume_id, bool dry_run) {
    userspace::write(console, dry_run ? "mkfat: would format " : "mkfat: formatted ");
    userspace::write(console, args.device);
    userspace::write_line(console, " as FAT32");
    userspace::write(console, "mkfat: sectors=");
    userspace::write_u64(console, layout.total_sectors);
    userspace::write(console, " sectors_per_cluster=");
    userspace::write_u64(console, layout.sectors_per_cluster);
    userspace::write(console, " reserved=");
    userspace::write_u64(console, layout.reserved_sectors);
    userspace::write(console, " fats=");
    userspace::write_u64(console, layout.num_fats);
    userspace::write(console, " fat_sectors=");
    userspace::write_u64(console, layout.fat_sectors);
    userspace::write(console, " clusters=");
    userspace::write_u64(console, layout.clusters);
    if (args.verbose) {
        userspace::write(console, " volume_id=");
        userspace::write_u64(console, volume_id);
    }
    userspace::write_line(console, "");
}

}  // namespace

int main(uint64_t arg_ptr, uint64_t) {
    long console = process_get_standard_descriptor(1);
    if (console < 0) console = descriptor_open(kDescConsole, 0);
    Args args{};
    if (!parse_args(reinterpret_cast<const char*>(arg_ptr), args)) {
        userspace::write_line(console, "mkfat: invalid arguments");
        userspace::write_line(console, "Try 'mkfat --help' for more information.");
        return 1;
    }
    if (args.show_help) { print_help(console); return 0; }
    if (args.show_version) {
        userspace::write_line(console, "mkfat (system-tools) 1.4.0");
        return 0;
    }
    long device = descriptor_open(
        kDescBlock,
        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(args.device)), 0, 0);
    if (device < 0) {
        userspace::write(console, "mkfat: unable to open ");
        userspace::write_line(console, args.device);
        return 1;
    }
    descriptor_defs::BlockGeometry geometry{};
    if (descriptor_get_property(
            static_cast<uint32_t>(device),
            static_cast<uint32_t>(descriptor_defs::Property::BlockGeometry),
            &geometry, sizeof(geometry)) != 0) {
        userspace::write_line(console, "mkfat: failed to query block geometry");
        descriptor_close(static_cast<uint32_t>(device));
        return 1;
    }
    Layout layout{};
    if (!calculate_layout(args, geometry, layout)) {
        userspace::write_line(console,
                              "mkfat: device or requested layout is not valid for FAT32 (512-byte sectors and at least 65525 clusters required)");
        descriptor_close(static_cast<uint32_t>(device));
        return 1;
    }
    if (!args.has_hidden_sectors) {
        descriptor_defs::PartitionInfo partition{};
        if (descriptor_get_property(
                static_cast<uint32_t>(device),
                static_cast<uint32_t>(descriptor_defs::Property::PartitionInfo),
                &partition, sizeof(partition)) == 0 &&
            partition.start_lba <= UINT32_MAX) {
            layout.hidden_sectors = static_cast<uint32_t>(partition.start_lba);
        }
    }
    if (!args.force && existing_filesystem(static_cast<uint32_t>(device))) {
        userspace::write_line(console,
                              "mkfat: existing filesystem signature found; use --force to overwrite");
        descriptor_close(static_cast<uint32_t>(device));
        return 1;
    }
    if (!args.dry_run &&
        descriptor_test_flag(static_cast<uint32_t>(device),
                             static_cast<uint64_t>(descriptor_defs::Flag::Writable)) != 1) {
        userspace::write_line(console, "mkfat: block device is not writable");
        descriptor_close(static_cast<uint32_t>(device));
        return 1;
    }
    uint32_t volume_id = args.has_volume_id
                             ? args.volume_id
                             : default_volume_id(args, layout);
    bool ok = args.dry_run || write_filesystem(static_cast<uint32_t>(device),
                                               args, layout, volume_id);
    if (!ok) userspace::write_line(console, "mkfat: failed while writing filesystem");
    else if (!args.quiet) print_layout(console, args, layout, volume_id,
                                      args.dry_run);
    descriptor_close(static_cast<uint32_t>(device));
    return ok ? 0 : 1;
}
