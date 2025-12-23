#ifndef MODUOS_FS_DEVFS_PATH_H
#define MODUOS_FS_DEVFS_PATH_H

// Small helper for parsing $/dev/<node>

static inline const char* devfs_strip_prefix(const char *p) {
    if (!p) return 0;
    if (p[0] == '$' && p[1] == '/' && p[2] == 'd' && p[3] == 'e' && p[4] == 'v') {
        const char *s = p + 5;
        while (*s == '/') s++;
        return s;
    }
    return 0;
}

#endif
