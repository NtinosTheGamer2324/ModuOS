#ifndef MODUOS_FS_PATH_NORM_H
#define MODUOS_FS_PATH_NORM_H

#include <stddef.h>

// Normalize a path in-place, removing duplicate '/', and resolving '.' and '..'.
// Supports both normal absolute paths (/...) and ModuOS virtual-root paths ($/...).
// For relative paths, caller should resolve to absolute first.
void path_normalize_inplace(char *path);

#endif
