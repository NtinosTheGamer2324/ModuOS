// hvfs.c
#include "moduos/fs/hvfs.h"
#include "moduos/fs/fs.h"
#include "moduos/kernel/COM/com.h"
#include "moduos/kernel/memory/memory.h"

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
    // Get mount (fix: fs_get_mount returns pointer, not struct)
    fs_mount_t* fmnt = fs_get_mount(drvmnt);
    
    if (!fmnt || !fmnt->valid) {
        com_write_string(COM1_PORT, "[HVFS] Invalid mount\n");
        return -1;
    }

    // Check if file exists
    if (!fs_file_exists(fmnt, path)) {
        return 1;
    }

    // Get file info
    fs_file_info_t info;
    if (fs_stat(fmnt, path, &info) != 0) {
        com_write_string(COM1_PORT, "[HVFS] fs_stat failed\n");
        return -1;
    }

    // Check if it's a directory
    if (info.is_directory) {
        return 2;
    }

    // Allocate buffer for file contents
    void *buffer = kmalloc(info.size);
    if (!buffer) {
        com_write_string(COM1_PORT, "[HVFS] kmalloc FAILED\n");
        return -3;
    }

    // Read the file
    size_t bytes_read = 0;
    int result = fs_read_file(fmnt, path, buffer, info.size, &bytes_read);
    
    if (result != 0 || bytes_read != info.size) {
        com_write_string(COM1_PORT, "[HVFS] fs_read_file FAILED\n");
        kfree(buffer);
        return -4;
    }

    // Return buffer and size
    *outbuf = buffer;
    if (out_size) {
        *out_size = bytes_read;
    }
    
    return 0;
}