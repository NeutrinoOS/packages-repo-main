#include <stddef.h>
#include <stdint.h>
#include <ctype.h>
#include <string.h>

#include "../crt/syscall.hpp"
#include "../auth/password_hash.hpp"
#include "keyboard_scancode.hpp"
#include "service_protocol.hpp"

namespace {

constexpr const char* kDefaultShellPath = "@sys/binary/shell.elf";
constexpr const char* kServiceDirectory = "@sys/config/services";
constexpr const char* kPrimaryUserStorePath = "/system/users.ntd";
constexpr const char* kFallbackUserStorePath = "/users.ntd";
constexpr size_t kConfigBufferSize = 1024;
constexpr size_t kMaxUserNameLength = 32;
constexpr size_t kMaxLoginUsers = 32;
constexpr const char* kRootUserName = "root";
constexpr uint64_t kAllCapabilities = ~0ull;
constexpr size_t kMaxServices = 24;
constexpr size_t kServiceDescriptionSize = 96;
constexpr size_t kServicePathSize = 192;
constexpr size_t kServiceArgsSize = 192;
constexpr size_t kServiceLogSize = 8192;

enum class RestartPolicy : uint8_t {
    Never,
    OnFailure,
    Always,
};

enum class ServiceType : uint8_t {
    Simple,
    Oneshot,
};

struct Service {
    char name[service_protocol::kNameSize];
    char description[kServiceDescriptionSize];
    char executable[kServicePathSize];
    char arguments[kServiceArgsSize];
    char user[kMaxUserNameLength];
    char after[service_protocol::kNameSize];
    RestartPolicy restart;
    ServiceType type;
    bool enabled;
    bool desired_running;
    bool restart_requested;
    bool completed;
    uint32_t pid;
    uint16_t exit_code;
    uint32_t starts;
    uint32_t log_handle;
    size_t log_start;
    size_t log_length;
    char log[kServiceLogSize];
};

uint32_t g_console_handle = kInvalidDescriptor;
bool g_allow_passwordless_root_bootstrap = false;

struct PrincipalCacheEntry {
    bool in_use;
    char name[kMaxUserNameLength];
    void* principal;
};

PrincipalCacheEntry g_principals[8]{};
Service g_services[kMaxServices]{};
size_t g_service_count = 0;
char g_service_response[service_protocol::kResponseTextSize];
ProcessEvent g_pending_authorizations[32]{};
size_t g_pending_authorization_count = 0;

struct UserStoreHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t entry_size;
    uint32_t count;
};

struct UserStoreHeaderV3 {
    uint32_t magic;
    uint16_t version;
    uint16_t entry_size;
    uint32_t count;
    uint64_t next_user_id;
    uint64_t machine_id;
};

struct PackedUserV1 {
    char name[kMaxUserNameLength];
    uint64_t allowed_caps;
    uint64_t generation;
    uint8_t active;
    uint8_t reserved[7];
};

struct PackedUser {
    char name[kMaxUserNameLength];
    uint64_t allowed_caps;
    uint64_t generation;
    uint8_t active;
    uint8_t password_set;
    uint16_t password_algorithm;
    uint32_t password_iterations;
    uint8_t password_salt[auth::kPasswordSaltSize];
    uint8_t password_hash[auth::kPasswordHashSize];
    uint8_t reserved[24];
};

struct LoginUsers {
    char names[kMaxLoginUsers][kMaxUserNameLength];
    bool password_set[kMaxLoginUsers];
    uint32_t password_iterations[kMaxLoginUsers];
    uint8_t password_salt[kMaxLoginUsers][auth::kPasswordSaltSize];
    uint8_t password_hash[kMaxLoginUsers][auth::kPasswordHashSize];
    size_t count;
};

constexpr size_t kUserStoreBufferSize =
    sizeof(UserStoreHeaderV3) +
    sizeof(PackedUser) * kMaxLoginUsers + 1;
constexpr uint32_t kMaxPasswordIterations =
    auth::kPasswordIterations * 10u;

void print(const char* text) {
    if (g_console_handle == kInvalidDescriptor || text == nullptr) {
        return;
    }
    size_t length = strlen(text);
    if (length != 0) {
        descriptor_write(g_console_handle, text, length);
    }
}

void print_char(char ch) {
    if (g_console_handle == kInvalidDescriptor) {
        return;
    }
    descriptor_write(g_console_handle, &ch, 1);
}

char* skip_spaces(char* text) {
    if (text == nullptr) {
        return nullptr;
    }
    while (*text != '\0' && isspace(*text)) {
        ++text;
    }
    return text;
}

void trim_trailing(char* start, char* end) {
    if (start == nullptr || end == nullptr || end < start) {
        return;
    }
    while (end > start) {
        char ch = *(end - 1);
        if (!isspace(ch)) {
            break;
        }
        --end;
    }
    *end = '\0';
}

void zero_memory(void* ptr, size_t size) {
    auto* bytes = static_cast<uint8_t*>(ptr);
    for (size_t i = 0; i < size; ++i) {
        bytes[i] = 0;
    }
}

bool copy_packed_user_name(const char* packed,
                           size_t packed_size,
                           char* out,
                           size_t out_size) {
    if (packed == nullptr || packed_size == 0 || out == nullptr ||
        out_size == 0) {
        return false;
    }
    size_t length = 0;
    while (length < packed_size && packed[length] != '\0') {
        unsigned char ch = static_cast<unsigned char>(packed[length]);
        if (ch < 0x20 || ch == 0x7F || ch == '/' || ch == '\\') {
            return false;
        }
        ++length;
    }
    if (length == 0 || length == packed_size || length >= out_size ||
        (length == 1 && packed[0] == '.') ||
        (length == 2 && packed[0] == '.' && packed[1] == '.')) {
        return false;
    }
    memcpy(out, packed, length);
    out[length] = '\0';
    return true;
}

void* find_cached_principal(const char* user_name) {
    if (user_name == nullptr || user_name[0] == '\0') {
        return nullptr;
    }
    for (size_t i = 0; i < sizeof(g_principals) / sizeof(g_principals[0]); ++i) {
        if (!g_principals[i].in_use) {
            continue;
        }
        if (strcmp(g_principals[i].name, user_name) == 0) {
            return g_principals[i].principal;
        }
    }
    return nullptr;
}

bool cache_principal(const char* user_name, void* principal) {
    if (user_name == nullptr || user_name[0] == '\0' || principal == nullptr) {
        return false;
    }
    for (size_t i = 0; i < sizeof(g_principals) / sizeof(g_principals[0]); ++i) {
        if (!g_principals[i].in_use) {
            g_principals[i].in_use = true;
            strlcpy(g_principals[i].name,
                    user_name,
                    sizeof(g_principals[i].name));
            g_principals[i].principal = principal;
            return true;
        }
    }
    return false;
}

void* ensure_user_principal(const char* user_name,
                            bool allow_root_bootstrap = false) {
    if (user_name == nullptr || user_name[0] == '\0') {
        return nullptr;
    }

    void* principal = find_cached_principal(user_name);
    if (principal != nullptr) {
        return principal;
    }

    void* user = user_find(user_name);
    if (user == nullptr && allow_root_bootstrap &&
        strcmp(user_name, kRootUserName) == 0) {
        user = user_create(user_name, kAllCapabilities);
    }
    if (user == nullptr) {
        return nullptr;
    }

    principal = principal_create(user, kAllCapabilities);
    if (principal == nullptr) {
        return nullptr;
    }
    cache_principal(user_name, principal);
    return principal;
}

bool activate_initial_root_principal(bool allow_root_bootstrap) {
    const char* user_name = kRootUserName;
    void* principal =
        ensure_user_principal(user_name, allow_root_bootstrap);
    if (principal == nullptr) {
        print("init: failed to ensure principal for ");
        print(user_name);
        print("\n");
        return false;
    }
    if (principal_set(principal) < 0) {
        print("init: failed to set principal for ");
        print(user_name);
        print("\n");
        return false;
    }
    return true;
}

bool read_file_into_buffer(const char* path,
                           char* buffer,
                           size_t buffer_size,
                           size_t& out_len) {
    out_len = 0;
    if (path == nullptr || buffer == nullptr || buffer_size == 0) {
        return false;
    }

    long handle = file_open(path);
    if (handle < 0) {
        return false;
    }

    size_t total = 0;
    while (total + 1 < buffer_size) {
        long read = file_read(static_cast<uint32_t>(handle),
                              buffer + total,
                              buffer_size - 1 - total);
        if (read <= 0) {
            break;
        }
        total += static_cast<size_t>(read);
    }

    file_close(static_cast<uint32_t>(handle));
    buffer[total] = '\0';
    out_len = total;
    return total > 0;
}

bool set_console_color(uint32_t fg, uint32_t bg) {
    descriptor_defs::ColorPair colors{fg, bg};
    return descriptor_set_property(
               g_console_handle,
               static_cast<uint32_t>(descriptor_defs::Property::ConsoleColor),
               &colors,
               sizeof(colors)) == 0;
}

bool user_exists(const LoginUsers& users, const char* name) {
    if (name == nullptr || name[0] == '\0') {
        return false;
    }
    for (size_t i = 0; i < users.count; ++i) {
        if (strcmp(users.names[i], name) == 0) {
            return true;
        }
    }
    return false;
}

void ensure_login_user(LoginUsers& users, const char* name) {
    if (name == nullptr || name[0] == '\0' || user_exists(users, name)) {
        return;
    }
    if (users.count >= kMaxLoginUsers) {
        return;
    }
    strlcpy(users.names[users.count], name, sizeof(users.names[users.count]));
    users.password_set[users.count] = false;
    users.password_iterations[users.count] = 0;
    zero_memory(users.password_salt[users.count], auth::kPasswordSaltSize);
    zero_memory(users.password_hash[users.count], auth::kPasswordHashSize);
    ++users.count;
}

void set_login_user_password(LoginUsers& users,
                             const char* name,
                             uint32_t iterations,
                             const uint8_t* salt,
                             const uint8_t* hash) {
    if (name == nullptr || salt == nullptr || hash == nullptr || iterations == 0) {
        return;
    }
    for (size_t i = 0; i < users.count; ++i) {
        if (strcmp(users.names[i], name) != 0) {
            continue;
        }
        users.password_set[i] = true;
        users.password_iterations[i] = iterations;
        memcpy(users.password_salt[i], salt, auth::kPasswordSaltSize);
        memcpy(users.password_hash[i], hash, auth::kPasswordHashSize);
        return;
    }
}

bool load_login_users(LoginUsers& users,
                      bool* out_explicit_empty = nullptr) {
    users.count = 0;
    if (out_explicit_empty != nullptr) {
        *out_explicit_empty = false;
    }

    char buffer[kUserStoreBufferSize];
    size_t len = 0;
    bool loaded = read_file_into_buffer(kPrimaryUserStorePath,
                                        buffer,
                                        sizeof(buffer),
                                        len);
    if (!loaded) {
        loaded = read_file_into_buffer(kFallbackUserStorePath,
                                       buffer,
                                       sizeof(buffer),
                                       len);
    }
    if (!loaded || len < sizeof(UserStoreHeader)) {
        return false;
    }

    UserStoreHeader header{};
    memcpy(&header, buffer, sizeof(header));
    if (header.magic != 0x4E544455u) {
        return false;
    }
    bool current_v2 = header.version == 2 &&
                      header.entry_size == sizeof(PackedUser);
    bool current_v3 = header.version == 3 &&
                      header.entry_size == sizeof(PackedUser);
    bool legacy = header.version == 1 &&
                  header.entry_size == sizeof(PackedUserV1);
    if (!current_v2 && !current_v3 && !legacy) {
        return false;
    }

    size_t available = 0;
    size_t count = header.count;
    if (current_v3) {
        if (len < sizeof(UserStoreHeaderV3)) {
            return false;
        }
        available = (len - sizeof(UserStoreHeaderV3)) / header.entry_size;
    } else {
        available = (len - sizeof(UserStoreHeader)) / header.entry_size;
    }
    if (count > available) {
        return false;
    }
    if (count > kMaxLoginUsers) {
        return false;
    }
    if (out_explicit_empty != nullptr && current_v3 && header.count == 0) {
        *out_explicit_empty = true;
    }

    if (legacy) {
        const PackedUserV1* entries =
            reinterpret_cast<const PackedUserV1*>(buffer + sizeof(UserStoreHeader));
        for (size_t i = 0; i < count; ++i) {
            if (entries[i].active == 0) {
                continue;
            }
            char name[kMaxUserNameLength];
            if (!copy_packed_user_name(entries[i].name,
                                       sizeof(entries[i].name),
                                       name,
                                       sizeof(name)) ||
                user_exists(users, name)) {
                return false;
            }
            ensure_login_user(users, name);
        }
    } else {
        const PackedUser* entries = reinterpret_cast<const PackedUser*>(
            buffer + (current_v3 ? sizeof(UserStoreHeaderV3) : sizeof(UserStoreHeader)));
        for (size_t i = 0; i < count; ++i) {
            if (entries[i].active == 0) {
                continue;
            }
            char name[kMaxUserNameLength];
            if (!copy_packed_user_name(entries[i].name,
                                       sizeof(entries[i].name),
                                       name,
                                       sizeof(name)) ||
                user_exists(users, name)) {
                return false;
            }
            ensure_login_user(users, name);
            if (entries[i].password_set != 0) {
                if (entries[i].password_algorithm !=
                        auth::kPasswordAlgorithmPbkdf2Sha256 ||
                    entries[i].password_iterations == 0 ||
                    entries[i].password_iterations > kMaxPasswordIterations) {
                    return false;
                }
                set_login_user_password(users,
                                        name,
                                        entries[i].password_iterations,
                                        entries[i].password_salt,
                                        entries[i].password_hash);
            }
        }
    }

    return users.count != 0;
}

bool read_login_name(char* out, size_t out_size) {
    if (out == nullptr || out_size == 0) {
        return false;
    }
    long keyboard = descriptor_open(
        static_cast<uint32_t>(descriptor_defs::Type::Keyboard), 0);
    if (keyboard < 0) {
        return false;
    }

    size_t length = 0;
    out[0] = '\0';
    for (;;) {
        descriptor_defs::KeyboardEvent events[8]{};
        long result = descriptor_read(static_cast<uint32_t>(keyboard),
                                      events,
                                      sizeof(events));
        if (result <= 0) {
            yield();
            continue;
        }
        size_t count = static_cast<size_t>(result) / sizeof(events[0]);
        for (size_t i = 0; i < count; ++i) {
            if (!keyboard::is_pressed(events[i]) ||
                keyboard::is_extended(events[i])) {
                continue;
            }
            char ch = keyboard::scancode_to_char(events[i].scancode,
                                                 events[i].mods);
            if (ch == '\r' || ch == '\n') {
                print("\n");
                descriptor_close(static_cast<uint32_t>(keyboard));
                out[length] = '\0';
                return length != 0;
            }
            if (ch == '\b' || ch == 0x7F) {
                if (length > 0) {
                    --length;
                    out[length] = '\0';
                    print("\b \b");
                }
                continue;
            }
            if (ch < 0x20 || ch > 0x7E) {
                continue;
            }
            if (length + 1 >= out_size) {
                continue;
            }
            out[length++] = ch;
            out[length] = '\0';
            print_char(ch);
        }
    }
}

bool read_secret_line(char* out, size_t out_size) {
    if (out == nullptr || out_size == 0) {
        return false;
    }
    long keyboard = descriptor_open(
        static_cast<uint32_t>(descriptor_defs::Type::Keyboard), 0);
    if (keyboard < 0) {
        return false;
    }

    size_t length = 0;
    out[0] = '\0';
    for (;;) {
        descriptor_defs::KeyboardEvent events[8]{};
        long result = descriptor_read(static_cast<uint32_t>(keyboard),
                                      events,
                                      sizeof(events));
        if (result <= 0) {
            yield();
            continue;
        }
        size_t count = static_cast<size_t>(result) / sizeof(events[0]);
        for (size_t i = 0; i < count; ++i) {
            if (!keyboard::is_pressed(events[i]) ||
                keyboard::is_extended(events[i])) {
                continue;
            }
            char ch = keyboard::scancode_to_char(events[i].scancode,
                                                 events[i].mods);
            if (ch == '\r' || ch == '\n') {
                print("\n");
                descriptor_close(static_cast<uint32_t>(keyboard));
                out[length] = '\0';
                return true;
            }
            if (ch == '\b' || ch == 0x7F) {
                if (length > 0) {
                    --length;
                    out[length] = '\0';
                }
                continue;
            }
            if (ch < 0x20 || ch > 0x7E) {
                continue;
            }
            if (length + 1 >= out_size) {
                continue;
            }
            out[length++] = ch;
            out[length] = '\0';
        }
    }
}

const LoginUsers* find_login_user(const LoginUsers& users,
                                  const char* name,
                                  size_t& out_index) {
    for (size_t i = 0; i < users.count; ++i) {
        if (strcmp(users.names[i], name) == 0) {
            out_index = i;
            return &users;
        }
    }
    return nullptr;
}

bool verify_login_password(const LoginUsers& users, size_t index) {
    if (index >= users.count) {
        return false;
    }
    if (!users.password_set[index]) {
        return g_allow_passwordless_root_bootstrap &&
               strcmp(users.names[index], kRootUserName) == 0;
    }
    print("password: ");
    char password[96];
    if (!read_secret_line(password, sizeof(password))) {
        return false;
    }
    uint8_t computed[auth::kPasswordHashSize];
    auth::pbkdf2_sha256(password,
                        users.password_salt[index],
                        auth::kPasswordSaltSize,
                        users.password_iterations[index],
                        computed);
    zero_memory(password, sizeof(password));
    bool ok = auth::constant_time_equal(computed,
                                        users.password_hash[index],
                                        auth::kPasswordHashSize);
    zero_memory(computed, sizeof(computed));
    return ok;
}

void build_shell_args(const char* user_name, char* out, size_t out_size) {
    if (out == nullptr || out_size == 0) {
        return;
    }
    strlcpy(out, "user=", out_size);
    size_t used = strlen(out);
    if (used + 1 >= out_size) {
        return;
    }
    strlcpy(out + used, user_name, out_size - used);
}

void run_login_loop() {
    set_console_color(0xFFF2F4F8u, 0x00000000u);
    for (;;) {
        LoginUsers users{};
        if (!load_login_users(users)) {
            print("\nLogin unavailable: credential store is missing or invalid.\n");
            sleep_ns(1000000000ull);
            continue;
        }
        print("\n");
        print("login: ");

        char name[kMaxUserNameLength];
        if (!read_login_name(name, sizeof(name))) {
            print("Login cancelled\n");
            continue;
        }
        if (!user_exists(users, name)) {
            print("Unknown user\n");
            continue;
        }
        size_t user_index = 0;
        if (find_login_user(users, name, user_index) == nullptr ||
            !verify_login_password(users, user_index)) {
            print("Login incorrect\n");
            continue;
        }
        void* principal = ensure_user_principal(name);
        if (principal == nullptr) {
            print("Failed to prepare user session\n");
            continue;
        }

        char shell_args[48];
        build_shell_args(name, shell_args, sizeof(shell_args));
        long result =
            exec_as(kDefaultShellPath, shell_args, 0, nullptr, principal);
        if (result < 0) {
            print("Failed to start shell\n");
        }
    }
}

bool valid_service_name(const char* name) {
    if (name == nullptr || name[0] == '\0') return false;
    size_t length = 0;
    for (; name[length] != '\0'; ++length) {
        char ch = name[length];
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') || ch == '-' || ch == '_')) {
            return false;
        }
        if (length + 1 >= service_protocol::kNameSize) return false;
    }
    return length != 0;
}

Service* find_service(const char* name) {
    if (name == nullptr) return nullptr;
    for (size_t i = 0; i < g_service_count; ++i) {
        if (strcmp(g_services[i].name, name) == 0) return &g_services[i];
    }
    return nullptr;
}

bool parse_bool(const char* value, bool& out) {
    if (strcmp(value, "true") == 0 || strcmp(value, "yes") == 0) {
        out = true;
        return true;
    }
    if (strcmp(value, "false") == 0 || strcmp(value, "no") == 0) {
        out = false;
        return true;
    }
    return false;
}

bool parse_service_file(const char* path, const char* filename, Service& service) {
    size_t filename_length = strlen(filename);
    constexpr char suffix[] = ".service";
    constexpr size_t suffix_length = sizeof(suffix) - 1;
    if (filename_length <= suffix_length ||
        strcmp(filename + filename_length - suffix_length, suffix) != 0 ||
        filename_length - suffix_length >= sizeof(service.name)) {
        return false;
    }

    service = Service{};
    memcpy(service.name, filename, filename_length - suffix_length);
    service.name[filename_length - suffix_length] = '\0';
    strlcpy(service.user, kRootUserName, sizeof(service.user));
    service.restart = RestartPolicy::OnFailure;
    service.log_handle = kInvalidDescriptor;

    char buffer[kConfigBufferSize];
    size_t length = 0;
    if (!read_file_into_buffer(path, buffer, sizeof(buffer), length)) return false;

    char* cursor = buffer;
    while (*cursor != '\0') {
        char* line = cursor;
        while (*cursor != '\0' && *cursor != '\n' && *cursor != '\r') ++cursor;
        char* end = cursor;
        while (*cursor == '\n' || *cursor == '\r') *cursor++ = '\0';
        line = skip_spaces(line);
        trim_trailing(line, end);
        if (line == nullptr || line[0] == '\0' || line[0] == '#') continue;

        char* equals = line;
        while (*equals != '\0' && *equals != '=') ++equals;
        if (*equals != '=') return false;
        char* key_end = equals;
        *equals++ = '\0';
        trim_trailing(line, key_end);
        char* value = skip_spaces(equals);
        if (strcmp(line, "description") == 0) {
            if (strlen(value) >= sizeof(service.description)) return false;
            strlcpy(service.description, value, sizeof(service.description));
        } else if (strcmp(line, "executable") == 0) {
            if (strlen(value) >= sizeof(service.executable)) return false;
            strlcpy(service.executable, value, sizeof(service.executable));
        } else if (strcmp(line, "arguments") == 0) {
            if (strlen(value) >= sizeof(service.arguments)) return false;
            strlcpy(service.arguments, value, sizeof(service.arguments));
        } else if (strcmp(line, "user") == 0) {
            if (strlen(value) >= sizeof(service.user)) return false;
            strlcpy(service.user, value, sizeof(service.user));
        } else if (strcmp(line, "after") == 0) {
            if (strlen(value) >= sizeof(service.after)) return false;
            strlcpy(service.after, value, sizeof(service.after));
        } else if (strcmp(line, "enabled") == 0) {
            if (!parse_bool(value, service.enabled)) return false;
        } else if (strcmp(line, "restart") == 0) {
            if (strcmp(value, "never") == 0) service.restart = RestartPolicy::Never;
            else if (strcmp(value, "on-failure") == 0) service.restart = RestartPolicy::OnFailure;
            else if (strcmp(value, "always") == 0) service.restart = RestartPolicy::Always;
            else return false;
        } else if (strcmp(line, "type") == 0) {
            if (strcmp(value, "simple") == 0) service.type = ServiceType::Simple;
            else if (strcmp(value, "oneshot") == 0) service.type = ServiceType::Oneshot;
            else return false;
        } else {
            return false;
        }
    }
    return valid_service_name(service.name) && service.executable[0] != '\0' &&
           valid_service_name(service.user) &&
           (service.after[0] == '\0' || valid_service_name(service.after));
}

void load_services() {
    long directory = directory_open(kServiceDirectory);
    if (directory < 0) {
        print("init: no service directory found\n");
        return;
    }
    DirEntry entry{};
    while (g_service_count < kMaxServices &&
           directory_read(static_cast<uint32_t>(directory), &entry) > 0) {
        if ((entry.flags & DIR_ENTRY_FLAG_DIRECTORY) != 0) continue;
        char path[256];
        strlcpy(path, kServiceDirectory, sizeof(path));
        strlcpy(path + strlen(path), "/", sizeof(path) - strlen(path));
        strlcpy(path + strlen(path), entry.name, sizeof(path) - strlen(path));
        Service parsed{};
        if (!parse_service_file(path, entry.name, parsed)) {
            print("init: invalid service definition ");
            print(entry.name);
            print("\n");
            continue;
        }
        if (find_service(parsed.name) != nullptr) {
            print("init: duplicate service ");
            print(parsed.name);
            print("\n");
            continue;
        }
        g_services[g_service_count++] = parsed;
    }
    directory_close(static_cast<uint32_t>(directory));
}

void append_log(Service& service, const char* data, size_t length) {
    for (size_t i = 0; i < length; ++i) {
        if (service.log_length < sizeof(service.log)) {
            size_t index = (service.log_start + service.log_length) % sizeof(service.log);
            service.log[index] = data[i];
            ++service.log_length;
        } else {
            service.log[service.log_start] = data[i];
            service.log_start = (service.log_start + 1) % sizeof(service.log);
        }
    }
}

void drain_service_log(Service& service) {
    if (service.log_handle == kInvalidDescriptor) return;
    char buffer[512];
    for (;;) {
        long result = descriptor_read(service.log_handle, buffer, sizeof(buffer));
        if (result == kDescriptorWouldBlock) return;
        if (result <= 0) {
            descriptor_close(service.log_handle);
            service.log_handle = kInvalidDescriptor;
            return;
        }
        append_log(service, buffer, static_cast<size_t>(result));
    }
}

bool start_service(Service& service, size_t depth = 0) {
    if (service.pid != 0) {
        service.desired_running = true;
        return true;
    }
    if (depth >= kMaxServices) return false;
    if (service.after[0] != '\0') {
        Service* dependency = find_service(service.after);
        if (dependency == nullptr || !start_service(*dependency, depth + 1)) return false;
    }
    void* principal = ensure_user_principal(service.user);
    if (principal == nullptr) return false;

    uint64_t read_flags = static_cast<uint64_t>(descriptor_defs::Flag::Readable) |
                          static_cast<uint64_t>(descriptor_defs::Flag::Async);
    long log_reader = pipe_open_new(read_flags);
    descriptor_defs::PipeInfo log_info{};
    if (log_reader < 0 ||
        pipe_get_info(static_cast<uint32_t>(log_reader), &log_info) != 0 ||
        log_info.id == 0) return false;
    uint64_t write_flags = static_cast<uint64_t>(descriptor_defs::Flag::Writable);
    long log_writer = pipe_open_existing(write_flags, log_info.id);
    long stdin_reader = pipe_open_new(
        static_cast<uint64_t>(descriptor_defs::Flag::Readable));
    if (log_writer < 0 || stdin_reader < 0) {
        descriptor_close(static_cast<uint32_t>(log_reader));
        if (log_writer >= 0) descriptor_close(static_cast<uint32_t>(log_writer));
        if (stdin_reader >= 0) descriptor_close(static_cast<uint32_t>(stdin_reader));
        return false;
    }
    ProcessStdioConfig stdio{
        static_cast<uint32_t>(stdin_reader),
        static_cast<uint32_t>(log_writer),
        static_cast<uint32_t>(log_writer),
        0,
    };
    const char* args = service.arguments[0] == '\0' ? nullptr : service.arguments;
    long pid = child_with_stdio_as(service.executable,
                                   args,
                                   0,
                                   nullptr,
                                   &stdio,
                                   principal);
    descriptor_close(static_cast<uint32_t>(stdin_reader));
    descriptor_close(static_cast<uint32_t>(log_writer));
    if (pid < 0) {
        descriptor_close(static_cast<uint32_t>(log_reader));
        return false;
    }
    service.log_handle = static_cast<uint32_t>(log_reader);
    service.pid = static_cast<uint32_t>(pid);
    service.desired_running = true;
    service.restart_requested = false;
    service.completed = false;
    ++service.starts;
    return true;
}

bool stop_service(Service& service, bool restart) {
    service.desired_running = restart;
    service.restart_requested = restart;
    if (!restart) service.completed = false;
    if (service.pid == 0) {
        return restart ? start_service(service) : true;
    }
    return process_control(service.pid, PROCESS_CONTROL_KILL) == 0;
}

void reap_service_children() {
    for (;;) {
        long result = process_wait_child(0, true);
        if (result < 0) return;
        uint32_t pid = static_cast<uint32_t>(result) >> 16;
        uint16_t exit_code = static_cast<uint16_t>(result);
        Service* service = nullptr;
        for (size_t i = 0; i < g_service_count; ++i) {
            if (g_services[i].pid == pid) {
                service = &g_services[i];
                break;
            }
        }
        if (service == nullptr) continue;
        drain_service_log(*service);
        service->pid = 0;
        service->exit_code = exit_code;
        service->completed = service->type == ServiceType::Oneshot &&
                             service->desired_running &&
                             !service->restart_requested &&
                             exit_code == 0;
        bool should_restart = service->restart_requested ||
            (service->desired_running && service->restart == RestartPolicy::Always) ||
            (service->desired_running && service->restart == RestartPolicy::OnFailure &&
             exit_code != 0);
        service->restart_requested = false;
        if (!should_restart) service->desired_running = false;
        if (should_restart) {
            append_log(*service, "\n[init: restarting service]\n", 28);
            (void)start_service(*service);
        }
    }
}

void append_text(size_t& length, const char* text) {
    while (text != nullptr && *text != '\0' &&
           length < sizeof(g_service_response)) {
        g_service_response[length++] = *text++;
    }
}

void append_uint(size_t& length, uint32_t value) {
    char digits[11];
    size_t count = 0;
    do {
        digits[count++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value != 0);
    while (count != 0 && length < sizeof(g_service_response)) {
        g_service_response[length++] = digits[--count];
    }
}

const char* service_state(const Service& service) {
    if (service.pid != 0 && service.restart_requested) return "restarting";
    if (service.pid != 0 && !service.desired_running) return "stopping";
    if (service.pid != 0) return "running";
    if (service.completed) return "completed";
    return "stopped";
}

const char* restart_policy_name(RestartPolicy policy) {
    if (policy == RestartPolicy::Always) return "always";
    if (policy == RestartPolicy::OnFailure) return "on-failure";
    return "never";
}

const char* service_type_name(ServiceType type) {
    return type == ServiceType::Oneshot ? "oneshot" : "simple";
}

void append_service_status(size_t& length, const Service& service, bool detail) {
    append_text(length, service.name);
    append_text(length, ": ");
    append_text(length, service_state(service));
    if (service.pid != 0) {
        append_text(length, " (pid ");
        append_uint(length, service.pid);
        append_text(length, ")");
    }
    if (detail) {
        append_text(length, "\n  executable: ");
        append_text(length, service.executable);
        append_text(length, "\n  user: ");
        append_text(length, service.user);
        append_text(length, "\n  type: ");
        append_text(length, service_type_name(service.type));
        append_text(length, "\n  enabled: ");
        append_text(length, service.enabled ? "yes" : "no");
        append_text(length, "\n  restart: ");
        append_text(length, restart_policy_name(service.restart));
        if (service.after[0] != '\0') {
            append_text(length, "\n  after: ");
            append_text(length, service.after);
        }
        append_text(length, "\n  starts: ");
        append_uint(length, service.starts);
        append_text(length, "\n  last exit: ");
        append_uint(length, service.exit_code);
        if (service.description[0] != '\0') {
            append_text(length, "\n  description: ");
            append_text(length, service.description);
        }
    }
    append_text(length, "\n");
}

void respond_to_request(const service_protocol::Request& request) {
    int32_t status = service_protocol::kStatusOk;
    size_t length = 0;
    Service* service = request.service[0] == '\0' ? nullptr : find_service(request.service);
    if (request.command == service_protocol::kList ||
        (request.command == service_protocol::kStatus && service == nullptr &&
         request.service[0] == '\0')) {
        for (size_t i = 0; i < g_service_count; ++i) {
            append_service_status(length, g_services[i], false);
        }
    } else if (service == nullptr) {
        status = service_protocol::kStatusNotFound;
        append_text(length, "service not found: ");
        append_text(length, request.service);
        append_text(length, "\n");
    } else if (request.command == service_protocol::kStatus) {
        append_service_status(length, *service, true);
    } else if (request.command == service_protocol::kStart) {
        if (!start_service(*service)) status = service_protocol::kStatusFailed;
        append_service_status(length, *service, false);
    } else if (request.command == service_protocol::kStop) {
        if (!stop_service(*service, false)) status = service_protocol::kStatusFailed;
        append_service_status(length, *service, false);
    } else if (request.command == service_protocol::kRestart) {
        if (!stop_service(*service, true)) status = service_protocol::kStatusFailed;
        append_service_status(length, *service, false);
    } else if (request.command == service_protocol::kLogs) {
        if (service->log_length == 0) {
            append_text(length, "(no output)\n");
        } else {
            for (size_t i = 0; i < service->log_length &&
                               length < sizeof(g_service_response); ++i) {
                g_service_response[length++] =
                    service->log[(service->log_start + i) % sizeof(service->log)];
            }
        }
    } else {
        status = service_protocol::kStatusInvalid;
        append_text(length, "invalid service command\n");
    }

    uint64_t flags = static_cast<uint64_t>(descriptor_defs::Flag::Writable);
    long reply = pipe_open_existing(flags, request.reply_pipe_id);
    if (reply < 0) return;
    service_protocol::ResponseHeader header{
        service_protocol::kMessageMagic,
        service_protocol::kMessageVersion,
        0,
        status,
        static_cast<uint32_t>(length),
    };
    (void)service_protocol::write_all(static_cast<uint32_t>(reply), &header, sizeof(header));
    if (length != 0) {
        (void)service_protocol::write_all(static_cast<uint32_t>(reply),
                                          g_service_response,
                                          length);
    }
    descriptor_close(static_cast<uint32_t>(reply));
}

bool authorize_request(const service_protocol::Request& request) {
    uint64_t expected = service_protocol::request_authorization(request);
    for (size_t i = 0; i < g_pending_authorization_count; ++i) {
        const ProcessEvent& event = g_pending_authorizations[i];
        if (event.type == service_protocol::kAuthorizationEvent &&
            event.sender_process_id == request.client_process_id &&
            event.value == expected) {
            for (size_t j = i + 1; j < g_pending_authorization_count; ++j) {
                g_pending_authorizations[j - 1] = g_pending_authorizations[j];
            }
            --g_pending_authorization_count;
            return true;
        }
    }
    for (;;) {
        ProcessEvent event{};
        long result = process_event_receive(&event, true);
        if (result != 0) return false;
        if (result == 0 &&
            event.type == service_protocol::kAuthorizationEvent &&
            event.sender_process_id == request.client_process_id &&
            event.value == expected) {
            return true;
        }
        if (event.type == service_protocol::kAuthorizationEvent) {
            if (g_pending_authorization_count ==
                sizeof(g_pending_authorizations) / sizeof(g_pending_authorizations[0])) {
                for (size_t i = 1; i < g_pending_authorization_count; ++i) {
                    g_pending_authorizations[i - 1] = g_pending_authorizations[i];
                }
                --g_pending_authorization_count;
            }
            g_pending_authorizations[g_pending_authorization_count++] = event;
        }
    }
}

void service_manager(void*) {
    uint64_t server_flags = static_cast<uint64_t>(descriptor_defs::Flag::Readable) |
                            static_cast<uint64_t>(descriptor_defs::Flag::Async);
    long server = pipe_open_new(server_flags);
    descriptor_defs::PipeInfo pipe_info{};
    if (server < 0 ||
        pipe_get_info(static_cast<uint32_t>(server), &pipe_info) != 0 ||
        pipe_info.id == 0) {
        print("init: failed to create service control pipe\n");
        return;
    }
    long registry_handle = shared_memory_open(service_protocol::kRegistryName,
                                               sizeof(service_protocol::Registry));
    descriptor_defs::SharedMemoryInfo registry_info{};
    if (registry_handle < 0 ||
        shared_memory_get_info(static_cast<uint32_t>(registry_handle),
                               &registry_info) != 0 ||
        registry_info.base == 0 ||
        registry_info.length < sizeof(service_protocol::Registry)) {
        print("init: failed to publish service manager\n");
        return;
    }
    auto* registry = reinterpret_cast<service_protocol::Registry*>(registry_info.base);
    *registry = service_protocol::Registry{
        service_protocol::kRegistryMagic,
        service_protocol::kRegistryVersion,
        pipe_info.id,
        process_id(),
    };

    for (size_t i = 0; i < g_service_count; ++i) {
        if (g_services[i].enabled) (void)start_service(g_services[i]);
    }

    for (;;) {
        for (size_t i = 0; i < g_service_count; ++i) drain_service_log(g_services[i]);
        reap_service_children();
        service_protocol::Request request{};
        if (service_protocol::read_all(static_cast<uint32_t>(server),
                                       &request,
                                       sizeof(request),
                                       false) &&
            request.magic == service_protocol::kMessageMagic &&
            request.version == service_protocol::kMessageVersion &&
            request.reply_pipe_id != 0 &&
            request.client_process_id != 0) {
            request.service[sizeof(request.service) - 1] = '\0';
            if (authorize_request(request)) {
                respond_to_request(request);
            }
        }
        sleep_ms(10);
    }
}

}  // namespace

int main(uint64_t, uint64_t) {
    long console = descriptor_open(
        static_cast<uint32_t>(descriptor_defs::Type::Console), 0);
    if (console >= 0) {
        g_console_handle = static_cast<uint32_t>(console);
    }

    // A valid, explicitly empty v3 store is the live/first-boot bootstrap
    // marker. Missing, malformed, or nonempty stores never authorize creating
    // a new root account, and the exception is local to this boot.
    LoginUsers startup_users{};
    bool explicit_empty_store = false;
    (void)load_login_users(startup_users, &explicit_empty_store);
    g_allow_passwordless_root_bootstrap = explicit_empty_store;

    if (!activate_initial_root_principal(explicit_empty_store)) {
        print("init: unable to establish the root security principal\n");
        for (;;) {
            sleep_ns(1000000000ull);
        }
    }
    load_services();
    if (thread_create(service_manager, nullptr) < 0) {
        print("init: failed to start service manager\n");
    }
    run_login_loop();
}
