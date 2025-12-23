#include "moduos/fs/path_norm.h"
#include "moduos/kernel/memory/string.h"

static int is_sep(char c) { return c == '/'; }

void path_normalize_inplace(char *path) {
    if (!path) return;

    // Detect prefix
    int is_virtual = (path[0] == '$' && path[1] == '/');
    size_t prefix_len = is_virtual ? 2 : 0;

    // We only normalize absolute paths: /... or $/...
    if (!(is_virtual || path[0] == '/')) return;

    // Tokenize manually into stack of segments
    const size_t max_segs = 64;
    const char *segs[max_segs];
    size_t seg_lens[max_segs];
    size_t n = 0;

    char *p = path + prefix_len;
    while (*p) {
        while (is_sep(*p)) p++;
        if (!*p) break;
        char *start = p;
        while (*p && !is_sep(*p)) p++;
        size_t len = (size_t)(p - start);

        if (len == 1 && start[0] == '.') {
            continue;
        }
        if (len == 2 && start[0] == '.' && start[1] == '.') {
            if (n > 0) n--; // pop
            continue;
        }
        if (n < max_segs) {
            segs[n] = start;
            seg_lens[n] = len;
            n++;
        }
    }

    // Rebuild
    char out[256];
    size_t oi = 0;
    if (is_virtual) {
        out[oi++] = '$';
        out[oi++] = '/';
    } else {
        out[oi++] = '/';
    }

    for (size_t i = 0; i < n; i++) {
        if (oi > 0 && out[oi - 1] != '/') out[oi++] = '/';
        size_t copy = seg_lens[i];
        if (oi + copy >= sizeof(out)) break;
        memcpy(out + oi, segs[i], copy);
        oi += copy;
        out[oi] = 0;
    }

    // Remove trailing slash (except root)
    if (oi > (is_virtual ? 2 : 1) && out[oi - 1] == '/') {
        out[oi - 1] = 0;
    }

    strncpy(path, out, 255);
    path[255] = 0;
}
