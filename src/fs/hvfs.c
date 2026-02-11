// hvfs.c
#include "moduos/fs/hvfs.h"
#include "moduos/fs/fs.h"
#include "moduos/fs/hvfs_cache.h"
#include "moduos/kernel/COM/com.h"
#include "moduos/kernel/memory/memory.h"

#ifndef HVFS_DEBUG
#define HVFS_DEBUG 0
#endif

#if HVFS_DEBUG
#define HVFS_LOG(s) com_write_string(COM1_PORT, s)
#else
#define HVFS_LOG(s) do { (void)(s); } while (0)
#endif

/*
HVFS - READ
 0 .. SUCCESS
-1 .. fs_stat FAILED
 1 .. File not found
 2 .. Path is a directory and NOT a FILE
-3 .. MALLOC FAIL
-4 .. fs_read_file FAILED
*/

int hvfs_read(int drvmnt, const char* path, void **outbuf, size_t *out_size) {
    fs_mount_t* fmnt = fs_get_mount(drvmnt);

    if (outbuf) *outbuf = NULL;
    if (out_size) *out_size = 0;

    if (!fmnt || !fmnt->valid) {
        HVFS_LOG("[HVFS] Invalid mount\n");
        return -1;
    }

    if (!path || !*path || !outbuf) {
        return -1;
    }

    // Get file info (single stat; fs_file_exists() would stat again)
    fs_file_info_t info;
    if (fs_stat(fmnt, path, &info) != 0) {
        /* Treat stat failure as "not found" for HVFS semantics */
        return 1;
    }

    if (info.is_directory) {
        return 2;
    }

    /* Cache lookup first */
    {
        void *cbuf = NULL;
        size_t csz = 0;
        if (hvfs_cache_lookup(drvmnt, path, &cbuf, &csz) == 1) {
            *outbuf = cbuf;
            if (out_size) *out_size = csz;
            return 0;
        }
    }

    void *buffer = kmalloc(info.size);
    if (!buffer) {
        HVFS_LOG("[HVFS] kmalloc FAILED\n");
        return -3;
    }

    size_t bytes_read = 0;
    int result = fs_read_file(fmnt, path, buffer, info.size, &bytes_read);

    if (result != 0 || bytes_read != info.size) {
        HVFS_LOG("[HVFS] fs_read_file FAILED\n");
        kfree(buffer);
        return -4;
    }

    /* Store in cache (may fail if cache full). */
    if (!hvfs_cache_store(drvmnt, path, buffer, bytes_read)) {
        *outbuf = buffer;
        if (out_size) *out_size = bytes_read;
        return 0;
    }

    *outbuf = buffer;
    if (out_size) *out_size = bytes_read;
    return 0;
}

void hvfs_free(int drvmnt, const char *path, void *buf) {
    if (!buf) return;
    if (hvfs_cache_release(drvmnt, path, buf)) return;
    kfree(buf);
}
