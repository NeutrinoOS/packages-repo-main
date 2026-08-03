#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void* memcpy(void* dest, const void* src, size_t count);
void* memset(void* dest, int value, size_t count);
void* memmove(void* dest, const void* src, size_t count);
int memcmp(const void* lhs, const void* rhs, size_t count);
int strcmp(const char* lhs, const char* rhs);
int strncmp(const char* lhs, const char* rhs, size_t count);
size_t strlen(const char* text);
size_t strlcpy(char* dest, const char* src, size_t size);
char* strcpy(char* dest, const char* src);
char* strncpy(char* dest, const char* src, size_t count);
char* strcat(char* dest, const char* src);
char* strchr(const char* text, int ch);
char* strrchr(const char* text, int ch);
char* strstr(const char* haystack, const char* needle);
int strcasecmp(const char* lhs, const char* rhs);
int strncasecmp(const char* lhs, const char* rhs, size_t count);
char* strerror(int error);

#ifdef __cplusplus
}
#endif
