#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _drmVersion {
    int version_major;
    int version_minor;
    int version_patchlevel;
    int name_len;
    char* name;
    int date_len;
    char* date;
    int desc_len;
    char* desc;
} drmVersion, *drmVersionPtr;

#define DRM_CAP_DUMB_BUFFER 0x1

int drmOpen(const char* name, const char* busid);
int drmClose(int fd);
int drmGetCap(int fd, uint64_t capability, uint64_t* value);
drmVersionPtr drmGetVersion(int fd);
void drmFreeVersion(drmVersionPtr version);

#ifdef __cplusplus
}
#endif
