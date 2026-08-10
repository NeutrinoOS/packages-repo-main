#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int is_root_path(const char* path) {
    if (path == NULL || *path != '/') {
        return 0;
    }
    while (*path == '/') {
        ++path;
    }
    return *path == '\0';
}

static int volume_alias(const char* name, char* alias, size_t alias_size) {
    char original[1024];
    char volume_path[258];
    char displayed_path[1024];
    size_t name_length = strlen(name);

    if (alias_size == 0 || name_length + 2 > sizeof(volume_path) ||
        getcwd(original, sizeof(original)) == NULL) {
        return 0;
    }

    volume_path[0] = '/';
    memcpy(volume_path + 1, name, name_length + 1);
    if (chdir(volume_path) < 0) {
        return 0;
    }

    int have_displayed_path =
        getcwd(displayed_path, sizeof(displayed_path)) != NULL;
    int restored = chdir(original) == 0;
    if (!have_displayed_path || !restored || displayed_path[0] != '@' ||
        strchr(displayed_path, '/') != NULL) {
        return 0;
    }

    size_t alias_length = strlen(displayed_path);
    if (alias_length + 1 > alias_size) {
        return 0;
    }
    memcpy(alias, displayed_path, alias_length + 1);
    return 1;
}

int main(int argc, char** argv) {
    const char* path = argc > 1 ? argv[1] : ".";
    int listing_root = is_root_path(path);
    DIR* directory = opendir(path);
    if (directory == NULL) {
        fprintf(stderr, "ls: unable to open %s: %s\n", path, strerror(errno));
        return 1;
    }

    int status = 0;
    errno = 0;
    for (;;) {
        struct dirent* entry = readdir(directory);
        if (entry == NULL) {
            if (errno != 0) {
                fprintf(stderr, "ls: unable to read %s: %s\n",
                        path,
                        strerror(errno));
                status = 1;
            }
            break;
        }
        char alias[65];
        if (listing_root && entry->d_type == DT_DIR &&
            volume_alias(entry->d_name, alias, sizeof(alias))) {
            printf("%s (%s)\n", entry->d_name, alias);
        } else {
            printf("%s%s\n",
                   entry->d_name,
                   entry->d_type == DT_DIR ? "/" : "");
        }
        errno = 0;
    }

    if (closedir(directory) < 0) {
        fprintf(stderr, "ls: unable to close %s: %s\n", path, strerror(errno));
        status = 1;
    }
    return status;
}
