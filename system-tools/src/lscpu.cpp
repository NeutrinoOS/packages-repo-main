#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "descriptors.hpp"
#include "../crt/syscall.hpp"

namespace {

constexpr uint32_t kDescConsole =
    static_cast<uint32_t>(descriptor_defs::Type::Console);
constexpr uint32_t kDescCpuInfo =
    static_cast<uint32_t>(descriptor_defs::Type::CpuInfo);

struct FeatureName {
    descriptor_defs::CpuFeature feature;
    const char* name;
};

constexpr FeatureName kFeatureNames[] = {
    {descriptor_defs::kCpuFeatureFpu, "fpu"},
    {descriptor_defs::kCpuFeatureTsc, "tsc"},
    {descriptor_defs::kCpuFeatureMsr, "msr"},
    {descriptor_defs::kCpuFeatureApic, "apic"},
    {descriptor_defs::kCpuFeaturePge, "pge"},
    {descriptor_defs::kCpuFeatureMmx, "mmx"},
    {descriptor_defs::kCpuFeatureFxsr, "fxsr"},
    {descriptor_defs::kCpuFeatureSse, "sse"},
    {descriptor_defs::kCpuFeatureSse2, "sse2"},
    {descriptor_defs::kCpuFeatureSse3, "sse3"},
    {descriptor_defs::kCpuFeatureSsse3, "ssse3"},
    {descriptor_defs::kCpuFeatureSse4_1, "sse4_1"},
    {descriptor_defs::kCpuFeatureSse4_2, "sse4_2"},
    {descriptor_defs::kCpuFeatureXsave, "xsave"},
    {descriptor_defs::kCpuFeatureAvx, "avx"},
    {descriptor_defs::kCpuFeatureAes, "aes"},
    {descriptor_defs::kCpuFeaturePclmulqdq, "pclmulqdq"},
    {descriptor_defs::kCpuFeatureRdrand, "rdrand"},
    {descriptor_defs::kCpuFeatureFsgsbase, "fsgsbase"},
    {descriptor_defs::kCpuFeatureSmep, "smep"},
    {descriptor_defs::kCpuFeatureSmap, "smap"},
    {descriptor_defs::kCpuFeatureInvpcid, "invpcid"},
    {descriptor_defs::kCpuFeatureRdseed, "rdseed"},
    {descriptor_defs::kCpuFeature1GiBPages, "pdpe1gb"},
    {descriptor_defs::kCpuFeatureNx, "nx"},
    {descriptor_defs::kCpuFeatureSyscall, "syscall"},
};

void print(long console, const char* text) {
    if (console >= 0 && text != nullptr) {
        descriptor_write(static_cast<uint32_t>(console), text, strlen(text));
    }
}

void print_u32(long console, uint32_t value) {
    char buffer[11];
    size_t pos = sizeof(buffer);
    buffer[--pos] = '\0';
    do {
        buffer[--pos] = static_cast<char>('0' + value % 10u);
        value /= 10u;
    } while (value != 0 && pos != 0);
    print(console, buffer + pos);
}

void print_field(long console, const char* name, const char* value) {
    print(console, name);
    print(console, ":");
    size_t name_length = strlen(name);
    while (name_length < 18) {
        print(console, " ");
        ++name_length;
    }
    print(console, value);
    print(console, "\n");
}

void print_numeric_field(long console, const char* name, uint32_t value) {
    print(console, name);
    print(console, ":");
    size_t name_length = strlen(name);
    while (name_length < 18) {
        print(console, " ");
        ++name_length;
    }
    print_u32(console, value);
    print(console, "\n");
}

bool has_feature(const uint8_t* bits,
                 size_t bit_bytes,
                 descriptor_defs::CpuFeature feature) {
    const uint32_t number = static_cast<uint32_t>(feature);
    const size_t byte = number / 8;
    return byte < bit_bytes && (bits[byte] & (1u << (number % 8))) != 0;
}

void print_features(long console, const uint8_t* bits, size_t bit_bytes) {
    print(console, "Flags:");
    size_t name_length = strlen("Flags");
    while (name_length < 18) {
        print(console, " ");
        ++name_length;
    }
    bool first = true;
    for (size_t i = 0; i < sizeof(kFeatureNames) / sizeof(kFeatureNames[0]); ++i) {
        if (!has_feature(bits, bit_bytes, kFeatureNames[i].feature)) continue;
        if (!first) print(console, " ");
        print(console, kFeatureNames[i].name);
        first = false;
    }
    print(console, "\n");
}

bool only_help_argument(const char* args) {
    if (args == nullptr) return false;
    while (*args == ' ' || *args == '\t' || *args == '\r' || *args == '\n') ++args;
    if (strcmp(args, "-h") == 0 || strcmp(args, "--help") == 0) return true;
    return false;
}

bool has_arguments(const char* args) {
    if (args == nullptr) return false;
    while (*args == ' ' || *args == '\t' || *args == '\r' || *args == '\n') ++args;
    return *args != '\0';
}

}  // namespace

int main(uint64_t arg_ptr, uint64_t) {
    long console = process_get_standard_descriptor(1);
    if (console < 0) console = descriptor_open(kDescConsole, 0);

    const char* args = reinterpret_cast<const char*>(arg_ptr);
    if (only_help_argument(args)) {
        print(console, "usage: lscpu\n");
        return 0;
    }
    if (has_arguments(args)) {
        print(console, "usage: lscpu\n");
        return 1;
    }

    long cpu_info = descriptor_open(kDescCpuInfo, 0);
    if (cpu_info < 0) {
        print(console, "lscpu: CPU information is unavailable\n");
        return 1;
    }
    descriptor_defs::CpuInfo info{};
    long bytes = descriptor_read(static_cast<uint32_t>(cpu_info),
                                 &info,
                                 sizeof(info),
                                 0);
    if (bytes != static_cast<long>(sizeof(info))) {
        descriptor_close(static_cast<uint32_t>(cpu_info));
        print(console, "lscpu: failed to read CPU information\n");
        return 1;
    }

    constexpr size_t kFeatureBytes =
        (static_cast<size_t>(descriptor_defs::kCpuFeatureCount) + 7) / 8;
    uint8_t feature_bits[kFeatureBytes]{};
    size_t feature_bytes =
        (static_cast<size_t>(info.feature_count) + 7) / 8;
    if (feature_bytes > sizeof(feature_bits)) feature_bytes = sizeof(feature_bits);
    if (feature_bytes != 0) {
        bytes = descriptor_read(static_cast<uint32_t>(cpu_info),
                                feature_bits,
                                feature_bytes,
                                sizeof(info));
        if (bytes < 0) {
            descriptor_close(static_cast<uint32_t>(cpu_info));
            print(console, "lscpu: failed to read CPU features\n");
            return 1;
        }
        feature_bytes = static_cast<size_t>(bytes);
    }
    descriptor_close(static_cast<uint32_t>(cpu_info));

    print_field(console, "Architecture", info.architecture);
    print_field(console, "CPU op-mode(s)", "64-bit");
    print_numeric_field(console, "CPU(s)", info.logical_cpus);
    print_field(console, "Vendor ID", info.vendor_id);
    print_field(console, "Model name", info.model_name);
    print_numeric_field(console, "CPU family", info.family);
    print_numeric_field(console, "Model", info.model);
    print_numeric_field(console, "Stepping", info.stepping);
    print_features(console, feature_bits, feature_bytes);
    return 0;
}
