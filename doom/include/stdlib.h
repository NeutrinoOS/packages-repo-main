#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void* malloc(size_t size);
void* calloc(size_t count, size_t size);
void* realloc(void* memory, size_t size);
void free(void* memory);
int atoi(const char* text);
long strtol(const char* text, char** end, int base);
unsigned long strtoul(const char* text, char** end, int base);
char* getenv(const char* name);
char* strdup(const char* text);
int abs(int value);
void exit(int status) __attribute__((noreturn));
void abort(void) __attribute__((noreturn));
int atexit(void (*function)(void));
int remove(const char* path);
int rename(const char* old_path, const char* new_path);
int system(const char* command);

#ifdef __cplusplus
}
#endif
