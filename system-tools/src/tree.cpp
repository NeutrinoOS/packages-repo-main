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
constexpr size_t kMaxPathLength = 256;
constexpr size_t kMaxDepth = 64;

struct Options {
    bool show_all;
    bool directories_only;
    size_t max_depth;
    char path[kMaxPathLength];
};

struct Totals {
    uint64_t directories;
    uint64_t files;
};

enum class NextResult {
    Entry,
    End,
    Error,
};

void usage(long console) {
    userspace::write_line(console,
        "usage: tree [-a] [-d] [-L depth] [--] [directory]");
}

bool parse_depth(const char* text, size_t& depth) {
    if (text == nullptr || text[0] == '\0') {
        return false;
    }

    size_t value = 0;
    for (size_t i = 0; text[i] != '\0'; ++i) {
        if (text[i] < '0' || text[i] > '9') {
            return false;
        }
        const size_t digit = static_cast<size_t>(text[i] - '0');
        if (value > (SIZE_MAX - digit) / 10) {
            return false;
        }
        value = value * 10 + digit;
    }
    if (value == 0 || value > kMaxDepth) {
        return false;
    }
    depth = value;
    return true;
}

bool parse_options(const char* raw, Options& options, bool& help) {
    options = {};
    options.max_depth = kMaxDepth;
    options.path[0] = '.';
    options.path[1] = '\0';
    help = false;

    const char* cursor = raw == nullptr ? "" : raw;
    bool options_enabled = true;
    bool have_path = false;
    bool expect_depth = false;
    char token[kMaxPathLength];

    while (!userspace::only_spaces_remain(cursor)) {
        if (!userspace::copy_token(cursor, token, sizeof(token))) {
            return false;
        }
        if (expect_depth) {
            if (!parse_depth(token, options.max_depth)) {
                return false;
            }
            expect_depth = false;
            continue;
        }
        if (options_enabled && strcmp(token, "--") == 0) {
            options_enabled = false;
            continue;
        }
        if (options_enabled && strcmp(token, "--help") == 0) {
            help = true;
            continue;
        }
        if (options_enabled && strcmp(token, "-L") == 0) {
            expect_depth = true;
            continue;
        }
        if (options_enabled && token[0] == '-' && token[1] != '\0') {
            for (size_t i = 1; token[i] != '\0'; ++i) {
                if (token[i] == 'a') {
                    options.show_all = true;
                } else if (token[i] == 'd') {
                    options.directories_only = true;
                } else {
                    return false;
                }
            }
            continue;
        }
        if (have_path) {
            return false;
        }
        memcpy(options.path, token, strlen(token) + 1);
        have_path = true;
    }

    return !expect_depth;
}

bool is_dot_entry(const char* name) {
    return strcmp(name, ".") == 0 || strcmp(name, "..") == 0;
}

NextResult next_visible(uint32_t directory,
                        const Options& options,
                        DirEntry& entry) {
    for (;;) {
        long result = directory_read(directory, &entry);
        if (result == 0) {
            return NextResult::End;
        }
        if (result < 0) {
            return NextResult::Error;
        }

        entry.name[sizeof(entry.name) - 1] = '\0';
        const bool is_directory =
            (entry.flags & DIR_ENTRY_FLAG_DIRECTORY) != 0;
        if (is_dot_entry(entry.name) ||
            (!options.show_all && entry.name[0] == '.') ||
            (options.directories_only && !is_directory)) {
            continue;
        }
        return NextResult::Entry;
    }
}

void print_prefix(long console,
                  const bool* ancestor_has_more,
                  size_t depth,
                  bool is_last) {
    for (size_t i = 0; i < depth; ++i) {
        userspace::write(console, ancestor_has_more[i] ? "│   " : "    ");
    }
    userspace::write(console, is_last ? "└── " : "├── ");
}

bool list_directory(long console,
                    uint32_t directory,
                    const Options& options,
                    size_t depth,
                    bool* ancestor_has_more,
                    Totals& totals) {
    DirEntry current{};
    NextResult current_result = next_visible(directory, options, current);
    if (current_result == NextResult::Error) {
        userspace::write_line(console, "[error reading directory]");
        return false;
    }

    bool ok = true;
    while (current_result == NextResult::Entry) {
        DirEntry next{};
        const NextResult next_result = next_visible(directory, options, next);
        const bool is_last = next_result != NextResult::Entry;
        const bool is_directory =
            (current.flags & DIR_ENTRY_FLAG_DIRECTORY) != 0;

        print_prefix(console, ancestor_has_more, depth, is_last);
        userspace::write(console, current.name);
        userspace::write_line(console, is_directory ? "/" : "");

        if (is_directory) {
            ++totals.directories;
            if (depth + 1 < options.max_depth) {
                long child = directory_open_at(directory, current.name);
                if (child < 0) {
                    ancestor_has_more[depth] = !is_last;
                    print_prefix(console, ancestor_has_more, depth + 1, true);
                    userspace::write_line(console, "[error opening directory]");
                    ok = false;
                } else {
                    ancestor_has_more[depth] = !is_last;
                    if (!list_directory(console,
                                        static_cast<uint32_t>(child),
                                        options,
                                        depth + 1,
                                        ancestor_has_more,
                                        totals)) {
                        ok = false;
                    }
                    if (directory_close(static_cast<uint32_t>(child)) < 0) {
                        ok = false;
                    }
                }
            }
        } else {
            ++totals.files;
        }

        if (next_result == NextResult::Error) {
            ancestor_has_more[depth] = false;
            print_prefix(console, ancestor_has_more, depth, true);
            userspace::write_line(console, "[error reading directory]");
            ok = false;
            break;
        }
        current = next;
        current_result = next_result;
    }
    return ok;
}

void print_totals(long console, const Totals& totals, bool directories_only) {
    userspace::write_line(console, "");
    userspace::write_u64(console, totals.directories);
    userspace::write(console,
        totals.directories == 1 ? " directory" : " directories");
    if (!directories_only) {
        userspace::write(console, ", ");
        userspace::write_u64(console, totals.files);
        userspace::write(console, totals.files == 1 ? " file" : " files");
    }
    userspace::write_line(console, "");
}

}  // namespace

int main(uint64_t arg_ptr, uint64_t) {
    long console = process_get_standard_descriptor(1);
    if (console < 0) {
        console = descriptor_open(kDescConsole, 0);
    }

    Options options{};
    bool help = false;
    if (!parse_options(reinterpret_cast<const char*>(arg_ptr), options, help)) {
        usage(console);
        return 1;
    }
    if (help) {
        usage(console);
        userspace::write_line(console, "  -a        include hidden entries");
        userspace::write_line(console, "  -d        list directories only");
        userspace::write_line(console, "  -L depth  limit recursion depth (1-64)");
        return 0;
    }

    long root = directory_open(options.path);
    if (root < 0) {
        userspace::write(console, "tree: unable to open ");
        userspace::write_line(console, options.path);
        return 1;
    }

    userspace::write_line(console, options.path);
    bool ancestor_has_more[kMaxDepth]{};
    Totals totals{};
    bool ok = list_directory(console,
                             static_cast<uint32_t>(root),
                             options,
                             0,
                             ancestor_has_more,
                             totals);
    if (directory_close(static_cast<uint32_t>(root)) < 0) {
        ok = false;
    }
    print_totals(console, totals, options.directories_only);
    return ok ? 0 : 1;
}
