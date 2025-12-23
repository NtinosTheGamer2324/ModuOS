#include "moduos/fs/path.h"
#include "moduos/kernel/memory/string.h"
#include "moduos/kernel/process/process.h"
#include "moduos/drivers/Drive/vDrive.h"

static int ascii_tolower(int c) {
    if (c >= 'A' && c <= 'Z') return c + 32;
    return c;
}

static int str_ieq(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        if (ascii_tolower(*a) != ascii_tolower(*b)) return 0;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

static void sanitize_drive_name(const char *in, char *out, size_t out_sz) {
    // Convert model string to path-safe: letters/digits => keep, spaces => '-', others => '-'
    if (!out || out_sz == 0) return;
    size_t j = 0;
    for (size_t i = 0; in && in[i] && j + 1 < out_sz; i++) {
        char c = in[i];
        if (c == ' ' || c == '\t') {
            if (j == 0 || out[j - 1] == '-') continue;
            out[j++] = '-';
        } else if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ) {
            out[j++] = c;
        } else {
            if (j == 0 || out[j - 1] == '-') continue;
            out[j++] = '-';
        }
    }
    // trim trailing '-'
    while (j > 0 && out[j - 1] == '-') j--;
    out[j] = 0;
}

static int devvfs_parse_drive(const char *name, int *out_vdrive_id, int *out_part_index) {
    if (!name || !*name || !out_vdrive_id) return -1;
    if (out_part_index) *out_part_index = 0;

    // Accept names like vDriveN or vDriveN-Px
    if ((name[0] == 'v' || name[0] == 'V') &&
        (name[1] == 'd' || name[1] == 'D') &&
        (name[2] == 'r' || name[2] == 'R') &&
        (name[3] == 'i' || name[3] == 'I') &&
        (name[4] == 'v' || name[4] == 'V') &&
        (name[5] == 'e' || name[5] == 'E')) {
        int n = 0;
        int ok = 0;
        int i = 6;
        for (; name[i]; i++) {
            if (name[i] == '-') break;
            if (name[i] < '0' || name[i] > '9') return -1;
            ok = 1;
            n = n * 10 + (name[i] - '0');
        }
        if (!ok) return -1;
        // Optional -P<digit>
        if (name[i] == '-') {
            if ((name[i+1] != 'p' && name[i+1] != 'P') || (name[i+2] < '0' || name[i+2] > '9')) {
                return -1;
            }
            if (out_part_index) *out_part_index = (name[i+2] - '0');
            i += 3;
            if (name[i] != 0) return -1;
        }
        *out_vdrive_id = n;
        return 0;
    }

    // Accept names like <anything>-vDriveN or <anything>-vDriveN-Px (legacy)
    for (int i = 0; name[i]; i++) {
        if (name[i] == '-') {
            const char *p = name + i + 1;
            if ((p[0] == 'v' || p[0] == 'V') &&
                (p[1] == 'd' || p[1] == 'D') &&
                (p[2] == 'r' || p[2] == 'R') &&
                (p[3] == 'i' || p[3] == 'I') &&
                (p[4] == 'v' || p[4] == 'V') &&
                (p[5] == 'e' || p[5] == 'E')) {
                int n = 0;
                int ok = 0;
                int j = 6;
                for (; p[j]; j++) {
                    if (p[j] == '-') break;
                    if (p[j] < '0' || p[j] > '9') { ok = 0; break; }
                    ok = 1;
                    n = n * 10 + (p[j] - '0');
                }
                if (!ok) continue;
                if (p[j] == '-') {
                    if ((p[j+1] != 'p' && p[j+1] != 'P') || (p[j+2] < '0' || p[j+2] > '9') || p[j+3] != 0) {
                        continue;
                    }
                    if (out_part_index) *out_part_index = (p[j+2] - '0');
                }
                *out_vdrive_id = n;
                return 0;
            }
        }
    }

    // match sanitized model name
    for (int i = 0; i < vdrive_get_count(); i++) {
        vdrive_t *d = vdrive_get(i);
        if (!d) continue;
        char san[64];
        sanitize_drive_name(d->model, san, sizeof(san));
        if (san[0] && str_ieq(san, name)) {
            *out_vdrive_id = i;
            return 0;
        }
    }

    return -1;
}

int fs_resolve_path(struct process *proc, const char *path, fs_path_resolved_t *out) {
    if (!proc || !path || !out) return -1;

    // Tolerate accidental leading whitespace in user-provided paths
    while (*path == ' ' || *path == '\t' || *path == '\r' || *path == '\n') path++;
    if (*path == 0) return -1;

    memset(out, 0, sizeof(*out));
    out->devvfs_drive = -1;
    out->mount_slot = -1;
    out->mount = NULL;

    // Default: current
    if (!(path[0] == '$' && path[1] == '/')) {
        out->route = FS_ROUTE_CURRENT;
        out->mount_slot = proc->current_slot;
        out->mount = fs_get_mount(proc->current_slot);
        strncpy(out->rel_path, path, sizeof(out->rel_path) - 1);
        out->rel_path[sizeof(out->rel_path) - 1] = 0;
        return (out->mount && out->mount->valid) ? 0 : -1;
    }

    // $/...
    const char *p = path + 2;
    while (*p == '/') p++;

    // $/ or $//... => DEVVFS root
    if (*p == 0) {
        out->route = FS_ROUTE_DEVVFS;
        out->devvfs_kind = 0;
        out->rel_path[0] = '/';
        out->rel_path[1] = 0;
        return 0;
    }

    // extract first component
    char comp[64] = {0};
    size_t ci = 0;
    while (p[ci] && p[ci] != '/' && ci + 1 < sizeof(comp)) {
        comp[ci] = p[ci];
        ci++;
    }
    comp[ci] = 0;
    const char *rest = p + ci;
    while (*rest == '/') rest++;

    if (comp[0] == 0) return -1;

    if (str_ieq(comp, "mnt")) {
        if (*rest == 0) {
            out->route = FS_ROUTE_DEVVFS;
            out->devvfs_kind = 1;
            return 0;
        }

        // next component = drive name
        char drv[64] = {0};
        size_t di = 0;
        while (rest[di] && rest[di] != '/' && di + 1 < sizeof(drv)) {
            drv[di] = rest[di];
            di++;
        }
        drv[di] = 0;
        const char *sub = rest + di;
        while (*sub == '/') sub++;

        int vdid;
        int want_part = 0;
        if (devvfs_parse_drive(drv, &vdid, &want_part) != 0) return -1;

        // Find mount slot for this vDrive (+ optional partition index)
        for (int slot = 0; slot < 26; slot++) {
            int vdrive_id = -1;
            uint32_t lba = 0;
            fs_type_t type;
            if (fs_get_mount_info(slot, &vdrive_id, &lba, &type) == 0) {
                if (vdrive_id != vdid) continue;

                if (want_part > 0) {
                    int pidx = fs_get_mount_partition_index(slot);
                    if (pidx != want_part) continue;
                }

                out->route = FS_ROUTE_MOUNT;
                out->mount_slot = slot;
                out->mount = fs_get_mount(slot);
                out->devvfs_drive = vdid;
                // Underlying FS expects absolute path
                out->rel_path[0] = '/';
                out->rel_path[1] = 0;
                if (*sub) {
                    strncat(out->rel_path, sub, sizeof(out->rel_path) - 2);
                }
                return (out->mount && out->mount->valid) ? 0 : -1;
            }
        }

        return -1;
    }

    if (str_ieq(comp, "dev")) {
        // DEVVFS
        //  - $/dev            => list devices (kind=2)
        //  - $/dev/input      => list input devices (kind=3)
        //  - $/dev/input/<n>  => open input device node (kind=3)
        out->route = FS_ROUTE_DEVVFS;

        // Parse next component under dev
        if (*rest) {
            char subc[64] = {0};
            size_t si = 0;
            while (rest[si] && rest[si] != '/' && si + 1 < sizeof(subc)) {
                subc[si] = rest[si];
                si++;
            }
            subc[si] = 0;
            const char *subrest = rest + si;
            while (*subrest == '/') subrest++;

            if (str_ieq(subc, "input")) {
                out->devvfs_kind = 3;
                out->rel_path[0] = '/';
                out->rel_path[1] = 0;
                if (*subrest) {
                    strncat(out->rel_path, subrest, sizeof(out->rel_path) - 2);
                }
                return 0;
            }

            if (str_ieq(subc, "graphics")) {
                out->devvfs_kind = 4;
                out->rel_path[0] = '/';
                out->rel_path[1] = 0;
                if (*subrest) {
                    strncat(out->rel_path, subrest, sizeof(out->rel_path) - 2);
                }
                return 0;
            }
        }

        out->devvfs_kind = 2;
        out->rel_path[0] = '/';
        out->rel_path[1] = 0;
        if (*rest) {
            strncat(out->rel_path, rest, sizeof(out->rel_path) - 2);
        }
        return 0;
    }

    return -1;
}
