// hvfs.h
#ifndef HVFS_H
#define HVFS_H

#include <stddef.h>

/**
 * Read entire file into memory
 * @param drvmnt: Mount slot index (0-25)
 * @param path: File path
 * @param outbuf: Output pointer to allocated buffer (caller must free)
 * @param out_size: Optional output for file size
 * @return: 0 on success, negative/positive error codes
 */
int hvfs_read(int drvmnt, const char* path, void **outbuf, size_t *out_size);

#endif // HVFS_H