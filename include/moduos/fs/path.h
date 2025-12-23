#ifndef MODUOS_FS_PATH_H
#define MODUOS_FS_PATH_H

#include <stdint.h>
#include <stddef.h>
#include "moduos/fs/fs.h"

struct process;

/* Path routing result */
typedef enum {
    FS_ROUTE_CURRENT = 0, /* current_slot mount */
    FS_ROUTE_DEVVFS  = 1, /* $/ virtual namespace */
    FS_ROUTE_MOUNT   = 2  /* explicit mount (returned in out_mount) */
} fs_route_t;

typedef struct {
    fs_route_t route;
    int mount_slot;            /* for FS_ROUTE_MOUNT or FS_ROUTE_CURRENT (filled), else -1 */
    fs_mount_t *mount;         /* optional pointer for convenience */
    char rel_path[256];        /* path passed to underlying FS (always absolute, starts with '/') */
    int devvfs_kind;           /* for DEVVFS: 1=mnt,2=dev */
    int devvfs_drive;          /* for DEVVFS mnt/dev drive index if applicable, else -1 */
} fs_path_resolved_t;

/* Resolve a global path into either: current mount, specific mount, or DEVVFS.
 * Supported:
 *  - /... => current mount, rel_path=/...
 *  - $/mnt/<drive-name>/... => mount for that drive (by vDrive model name, case-insensitive)
 *  - $/mnt/vDriveN/... => mount for that vDrive
 *  - $/mnt => DEVVFS list of mounts
 *  - $/dev => DEVVFS list of devices
 */
int fs_resolve_path(struct process *proc, const char *path, fs_path_resolved_t *out);

#endif
