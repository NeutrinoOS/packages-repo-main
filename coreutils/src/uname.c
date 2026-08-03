#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/utsname.h>

enum field {
    FIELD_SYSNAME = 1u << 0,
    FIELD_NODENAME = 1u << 1,
    FIELD_RELEASE = 1u << 2,
    FIELD_VERSION = 1u << 3,
    FIELD_MACHINE = 1u << 4,
    FIELD_ALL = FIELD_SYSNAME | FIELD_NODENAME | FIELD_RELEASE |
                FIELD_VERSION | FIELD_MACHINE,
};

struct field_value {
    unsigned int flag;
    const char* value;
};

static void print_usage(FILE* output) {
    fputs("usage: uname [-asnrvm]\n", output);
}

static void print_help(void) {
    print_usage(stdout);
    fputs("Print system information. With no option, the system name is printed.\n"
          "\n"
          "  -a, --all             print all information\n"
          "  -s, --kernel-name     print the system name\n"
          "  -n, --nodename        print the network node name\n"
          "  -r, --kernel-release  print the kernel release\n"
          "  -v, --kernel-version  print the kernel version\n"
          "  -m, --machine         print the machine hardware name\n"
          "      --help            display this help\n"
          "      --version         display command version\n",
          stdout);
}

static int parse_short_options(const char* argument, unsigned int* selected) {
    for (size_t index = 1; argument[index] != '\0'; ++index) {
        switch (argument[index]) {
            case 'a':
                *selected |= FIELD_ALL;
                break;
            case 's':
                *selected |= FIELD_SYSNAME;
                break;
            case 'n':
                *selected |= FIELD_NODENAME;
                break;
            case 'r':
                *selected |= FIELD_RELEASE;
                break;
            case 'v':
                *selected |= FIELD_VERSION;
                break;
            case 'm':
                *selected |= FIELD_MACHINE;
                break;
            default:
                fprintf(stderr, "uname: invalid option -- '%c'\n",
                        argument[index]);
                return -1;
        }
    }
    return 0;
}

static int parse_long_option(const char* argument, unsigned int* selected) {
    static const struct field_value options[] = {
        {FIELD_ALL, "--all"},
        {FIELD_SYSNAME, "--kernel-name"},
        {FIELD_NODENAME, "--nodename"},
        {FIELD_RELEASE, "--kernel-release"},
        {FIELD_VERSION, "--kernel-version"},
        {FIELD_MACHINE, "--machine"},
    };

    for (size_t index = 0; index < sizeof(options) / sizeof(options[0]);
         ++index) {
        if (strcmp(argument, options[index].value) == 0) {
            *selected |= options[index].flag;
            return 0;
        }
    }
    fprintf(stderr, "uname: unrecognized option '%s'\n", argument);
    return -1;
}

int main(int argc, char** argv) {
    unsigned int selected = 0;
    for (int index = 1; index < argc; ++index) {
        const char* argument = argv[index];
        if (strcmp(argument, "--help") == 0) {
            print_help();
            return 0;
        }
        if (strcmp(argument, "--version") == 0) {
            puts("uname (Neutrino) development");
            return 0;
        }
        if (argument[0] != '-' || argument[1] == '\0') {
            fprintf(stderr, "uname: extra operand '%s'\n", argument);
            print_usage(stderr);
            return 1;
        }
        int result = argument[1] == '-'
                         ? parse_long_option(argument, &selected)
                         : parse_short_options(argument, &selected);
        if (result < 0) {
            print_usage(stderr);
            return 1;
        }
    }

    if (selected == 0) {
        selected = FIELD_SYSNAME;
    }

    struct utsname information;
    if (uname(&information) < 0) {
        perror("uname");
        return 1;
    }
    const struct field_value fields[] = {
        {FIELD_SYSNAME, information.sysname},
        {FIELD_NODENAME, information.nodename},
        {FIELD_RELEASE, information.release},
        {FIELD_VERSION, information.version},
        {FIELD_MACHINE, information.machine},
    };

    bool needs_separator = false;
    for (size_t index = 0; index < sizeof(fields) / sizeof(fields[0]); ++index) {
        if ((selected & fields[index].flag) == 0) {
            continue;
        }
        if (needs_separator) {
            putchar(' ');
        }
        fputs(fields[index].value, stdout);
        needs_separator = true;
    }
    putchar('\n');
    return ferror(stdout) ? 1 : 0;
}
