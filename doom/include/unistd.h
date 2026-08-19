#pragma once

#include <stddef.h>

#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

#ifdef __cplusplus
extern "C" {
#endif
int isatty(int fd);
#ifdef __cplusplus
}
#endif
