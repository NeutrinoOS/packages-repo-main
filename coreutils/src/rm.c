#include <errno.h>
#include <dirent.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

enum { kMaxPathLength = 1024 };

static int join_path(const char* parent,
                     const char* name,
                     char* output,
                     size_t output_size) {
    size_t parent_length = strlen(parent);
    size_t name_length = strlen(name);
    int separator_needed = parent_length != 0 &&
                         parent[parent_length - 1] != '/';
    if (parent_length > SIZE_MAX - name_length - (size_t)separator_needed - 1 ||
        parent_length + (size_t)separator_needed + name_length + 1 > output_size) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(output, parent, parent_length);
    if (separator_needed) {
        output[parent_length++] = '/';
    }
    memcpy(output + parent_length, name, name_length + 1);
    return 0;
}

static int remove_recursive_path(const char* path);

static int remove_tree(const char* path) {
    DIR* directory = opendir(path);
    if (directory == NULL) {
        return -1;
    }

    int status = 0;
    for (;;) {
        errno = 0;
        struct dirent* entry = readdir(directory);
        if (entry == NULL) {
            if (errno != 0) {
                status = -1;
            }
            break;
        }
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        char child[kMaxPathLength];
        if (join_path(path, entry->d_name, child, sizeof(child)) < 0) {
            status = -1;
            break;
        }

        if (remove_recursive_path(child) < 0) {
            status = -1;
            break;
        }
    }

    int saved_errno = errno;
    if (closedir(directory) < 0 && status == 0) {
        return -1;
    }
    if (status < 0) {
        errno = saved_errno;
    }
    return status;
}

static int remove_recursive_path(const char* path) {
    if (unlink(path) == 0) {
        return 0;
    }
    if (errno != EISDIR || remove_tree(path) < 0) {
        return -1;
    }
    return rmdir(path);
}

int main(int argc, char** argv) {
    int recursive = 0;
    const char* path = NULL;
    if (argc == 2) {
        path = argv[1];
    } else if (argc == 3 && strcmp(argv[1], "-r") == 0) {
        recursive = 1;
        path = argv[2];
    } else {
        fputs("usage: rm [-r] <path>\n", stderr);
        return 1;
    }

    if (recursive && remove_recursive_path(path) == 0) {
        return 0;
    }
    if (!recursive && (unlink(path) == 0 || rmdir(path) == 0)) {
        return 0;
    }
    fprintf(stderr, "rm: unable to remove %s: %s\n",
            path,
            strerror(errno));
    return 1;
}
