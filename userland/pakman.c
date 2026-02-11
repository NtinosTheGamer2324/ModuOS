#include "libc.h"
#include "string.h"
#include "lib_json.h"
#include "lib_pakzip.h"

// Pakman installs ZIP-based .pak files and tracks installed packages in JSON.
// No internet access on ModuOS: --web is not supported.


#define MAX_PKGS 256

static const char *g_root = "/";
static char g_root_buf[256] = {0};

static void set_root(const char *r) {
    if (!r || !r[0]) r = "/";
    strncpy(g_root_buf, r, sizeof(g_root_buf)-1);
    g_root_buf[sizeof(g_root_buf)-1] = 0;
    g_root = g_root_buf;
}


static void write_line(const char *s) {
    puts_raw(s);
    puts_raw("\n");
}

static void print_usage(void) {
    write_line("pakman:");
    write_line("  pakman install <path_to_pak> [--root=/path]");
    write_line("  pakman install --web <url>   (no internet access on ModuOS)");
    write_line("  pakman uninstall <name> [--root=/path]");
    write_line("  pakman list [--root=/path]");
    write_line("  pakman validate <path_to_pak>");
    write_line("  pakman createpakfile <folder> <out.pak>");
    write_line("  pakman changeroot <root>");
}

static void join_path(const char *a, const char *b, char *out, int outcap) {
    if (!a || !b || !out || outcap <= 0) return;
    int al = strlen(a);
    int bl = strlen(b);
    int pos = 0;
    for (int i=0;i<al && pos<outcap-1;i++) out[pos++] = a[i];
    if (pos>0 && out[pos-1] != '/' && pos < outcap-1) out[pos++] = '/';
    for (int i=0;i<bl && pos<outcap-1;i++) out[pos++] = b[i];
    out[pos] = 0;
}

static void get_manifest_path(char *out, int cap) {
    // <root>/appdata/pakman/installed_packages.json
    char tmp[256];
    join_path(g_root, "appdata/pakman", tmp, sizeof(tmp));
    join_path(tmp, "installed_packages.json", out, cap);
}

static void ensure_manifest_dir(void) {
    char p1[256];
    join_path(g_root, "appdata", p1, sizeof(p1));
    mkdir(p1);
    join_path(g_root, "appdata/pakman", p1, sizeof(p1));
    mkdir(p1);
    join_path(g_root, "appdata/pakman/packages", p1, sizeof(p1));
    mkdir(p1);
}

static int read_all(const char *path, char *buf, int cap) {
    int fd = open(path, 0, 0);
    if (fd < 0) return -1;
    int off = 0;
    while (off < cap-1) {
        int r = read(fd, buf + off, cap-1-off);
        if (r <= 0) break;
        off += r;
    }
    buf[off] = 0;
    close(fd);
    return off;
}

static int write_all(const char *path, const char *buf, int len) {
    int fd = open(path, 0x2 | 0x40 | 0x200, 0); // crude: O_RDWR|O_CREAT|O_TRUNC (depends on libc.h)
    if (fd < 0) return -1;
    int off=0;
    while (off < len) {
        int w = write(fd, buf+off, len-off);
        if (w <= 0) break;
        off += w;
    }
    close(fd);
    return (off==len)?0:-1;
}

static int load_manifest(pakman_pkg_t *pkgs, int max_pkgs) {
    char mp[256];
    get_manifest_path(mp, sizeof(mp));
    char json[4096];
    int n = read_all(mp, json, sizeof(json));
    if (n < 0) return 0; // no manifest
    int c = json_manifest_parse(json, (size_t)n, pkgs, max_pkgs);
    if (c < 0) return 0;
    return c;
}

static int save_manifest(const pakman_pkg_t *pkgs, int count) {
    char out[4096];
    int n = json_manifest_write(pkgs, count, out, sizeof(out));
    if (n < 0) return -1;
    char mp[256];
    get_manifest_path(mp, sizeof(mp));
    return write_all(mp, out, n);
}

static void load_saved_root(void) {
    // saved root path is stored in /appdata/pakman/root.txt (global, not per root)
    char buf[256];
    int n = read_all("/appdata/pakman/root.txt", buf, sizeof(buf));
    if (n > 0) {
        // trim whitespace
        while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r' || buf[n-1] == ' ' || buf[n-1] == '\t')) {
            buf[n-1] = 0; n--;
        }
        if (buf[0]) set_root(buf);
    }
}

static void parse_root_arg(int argc, char **argv) {
    load_saved_root();
    for (int i=1;i<argc;i++) {
        if (strncmp(argv[i], "--root=", 7) == 0) {
            set_root(argv[i] + 7);
        }
    }
}

static int rm_rf(const char *path);

static int copy_file(const char *src, const char *dst, unsigned int size_hint) {
    int in = open(src, 0, 0);
    if (in < 0) return -1;
    int out = open(dst, 0x2 | 0x40 | 0x200, 0);
    if (out < 0) { close(in); return -1; }

    char buf[4096];
    while (1) {
        int r = read(in, buf, sizeof(buf));
        if (r <= 0) break;
        int off = 0;
        while (off < r) {
            int w = write(out, buf + off, r - off);
            if (w <= 0) { close(in); close(out); return -1; }
            off += w;
        }
    }
    close(in);
    close(out);
    (void)size_hint;
    return 0;
}

static int copy_dir_recursive(const char *src_dir, const char *dst_dir) {
    mkdir(dst_dir);

    int dfd = opendir(src_dir);
    if (dfd < 0) return -1;

    while (1) {
        char name[260];
        int is_dir = 0;
        unsigned int size = 0;
        int r = readdir(dfd, name, sizeof(name), &is_dir, &size);
        if (r == 0) break;
        if (r < 0) { closedir(dfd); return -1; }
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;

        char src[512];
        char dst[512];
        join_path(src_dir, name, src, sizeof(src));
        join_path(dst_dir, name, dst, sizeof(dst));

        if (is_dir) {
            if (copy_dir_recursive(src, dst) != 0) { closedir(dfd); return -1; }
        } else {
            if (copy_file(src, dst, size) != 0) { closedir(dfd); return -1; }
        }
    }

    closedir(dfd);
    return 0;
}

static int rm_rf(const char *path) {
    int dfd = opendir(path);
    if (dfd >= 0) {
        // directory
        while (1) {
            char name[260];
            int is_dir = 0;
            unsigned int size = 0;
            int r = readdir(dfd, name, sizeof(name), &is_dir, &size);
            if (r == 0) break;
            if (r < 0) { closedir(dfd); return -1; }
            if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
            char child[512];
            join_path(path, name, child, sizeof(child));
            if (is_dir) {
                if (rm_rf(child) != 0) { closedir(dfd); return -1; }
            } else {
                unlink(child);
            }
        }
        closedir(dfd);
        rmdir(path);
        return 0;
    }
    // file
    unlink(path);
    return 0;
}

static int parse_pakmeta(const char *meta_path, char *name, int ncap, char *ver, int vcap, char *desc, int dcap) {
    char buf[1024];
    int n = read_all(meta_path, buf, sizeof(buf));
    if (n < 0) return -1;
    name[0]=0; ver[0]=0; desc[0]=0;

    const char *p = buf;
    while (*p) {
        // read line
        char line[256];
        int li=0;
        while (*p && *p!='\n' && li<(int)sizeof(line)-1) line[li++]=*p++;
        if (*p=='\n') p++;
        line[li]=0;
        // trim
        char *t=line;
        while (*t==' '||*t=='\t'||*t=='\r') t++;
        if (!*t) continue;
        char *colon = NULL;
        for (char *q=t; *q; q++) if (*q==':') { colon=q; break; }
        if (!colon) continue;
        *colon=0;
        char *key=t;
        char *val=colon+1;
        while (*val==' '||*val=='\t') val++;
        // strip quotes
        if (*val=='\"') { val++; char *e=val; while(*e && *e!='\"') e++; *e=0; }

        if (strcmp(key,"name") == 0 || strcmp(key,"Name") == 0) {
            strncpy(name,val,ncap-1); name[ncap-1]=0;
        } else if (strcmp(key,"version") == 0 || strcmp(key,"Version") == 0) {
            strncpy(ver,val,vcap-1); ver[vcap-1]=0;
        } else if (strcmp(key,"description") == 0 || strcmp(key,"Description") == 0) {
            strncpy(desc,val,dcap-1); desc[dcap-1]=0;
        }
    }

    return name[0] ? 0 : -1;
}

static int cmd_list(int argc, char **argv) {
    (void)argc; (void)argv;
    ensure_manifest_dir();
    pakman_pkg_t pkgs[MAX_PKGS];
    int cnt = load_manifest(pkgs, MAX_PKGS);
    if (cnt <= 0) {
        write_line("No packages installed.");
        return 0;
    }
    for (int i=0;i<cnt;i++) {
        puts_raw("- "); puts_raw(pkgs[i].name);
        puts_raw(" v"); puts_raw(pkgs[i].version);
        puts_raw(" ("); puts_raw(pkgs[i].install_path); puts_raw(")\n");
    }
    return 0;
}

int md_main(long argc, char **argv) {
    parse_root_arg(argc, argv);
    if (argc < 2) { print_usage(); return 0; }

    const char *cmd = argv[1];

    if (strcmp(cmd, "list") == 0) return cmd_list(argc, argv);

    if (strcmp(cmd, "changeroot") == 0) {
        if (argc < 3) { write_line("pakman changeroot <root>"); return 1; }
        // Persist to /appdata/pakman/root.txt
        mkdir("/appdata");
        mkdir("/appdata/pakman");
        set_root(argv[2]);
        write_all("/appdata/pakman/root.txt", g_root, strlen(g_root));
        write_all("/appdata/pakman/root.txt", "\n", 1);
        puts_raw("Root set to: "); write_line(g_root);
        return 0;
    }

    if (strcmp(cmd, "validate") == 0) {
        if (argc < 3) { write_line("pakman validate <file.pak>"); return 1; }
        const char *pak = argv[2];
        const char *tmp = "/appdata/pakman/tmp_extract";
        rm_rf(tmp);
        mkdir("/appdata"); mkdir("/appdata/pakman"); mkdir(tmp);
        if (pakzip_extract(pak, tmp) != 0) { write_line("validate: extract failed"); return 1; }
        char meta[512];
        join_path(tmp, "info.pakmeta", meta, sizeof(meta));
        char name[64], ver[32], desc[128];
        if (parse_pakmeta(meta, name, sizeof(name), ver, sizeof(ver), desc, sizeof(desc)) != 0) {
            write_line("validate: info.pakmeta missing/invalid");
            return 1;
        }
        puts_raw("Valid: "); puts_raw(name); puts_raw(" v"); write_line(ver);
        return 0;
    }

    if (strcmp(cmd, "install") == 0) {
        if (argc >= 4 && strcmp(argv[2], "--web") == 0) {
            write_line("pakman: no internet access on ModuOS");
            return 2;
        }
        if (argc < 3) { write_line("pakman install <file.pak> [--root=...]"); return 1; }
        ensure_manifest_dir();
        const char *pak = argv[2];
        const char *tmp = "/appdata/pakman/tmp_extract";
        rm_rf(tmp);
        mkdir("/appdata"); mkdir("/appdata/pakman"); mkdir(tmp);

        if (pakzip_extract(pak, tmp) != 0) { write_line("install: extract failed"); return 1; }

        char meta[512];
        join_path(tmp, "info.pakmeta", meta, sizeof(meta));
        char name[64], ver[32], desc[128];
        if (parse_pakmeta(meta, name, sizeof(name), ver, sizeof(ver), desc, sizeof(desc)) != 0) {
            write_line("install: info.pakmeta missing/invalid");
            return 1;
        }

        // install path
        char pkroot[256];
        join_path(g_root, "appdata/pakman/packages", pkroot, sizeof(pkroot));
        char dst[512];
        join_path(pkroot, name, dst, sizeof(dst));

        // overwrite existing
        rm_rf(dst);
        mkdir(dst);
        if (copy_dir_recursive(tmp, dst) != 0) { write_line("install: copy failed"); return 1; }

        // update manifest
        pakman_pkg_t pkgs[MAX_PKGS];
        int cnt = load_manifest(pkgs, MAX_PKGS);
        for (int i=0;i<cnt;i++) {
            if (strcmp(pkgs[i].name, name) == 0) {
                write_line("install: already installed");
                return 0;
            }
        }
        if (cnt < MAX_PKGS) {
            strncpy(pkgs[cnt].name, name, sizeof(pkgs[cnt].name)-1);
            strncpy(pkgs[cnt].version, ver, sizeof(pkgs[cnt].version)-1);
            strncpy(pkgs[cnt].install_path, dst, sizeof(pkgs[cnt].install_path)-1);
            cnt++;
            save_manifest(pkgs, cnt);
        }

        puts_raw("Installed: "); puts_raw(name); puts_raw(" -> "); write_line(dst);
        return 0;
    }

    if (strcmp(cmd, "uninstall") == 0) {
        if (argc < 3) { write_line("pakman uninstall <name> [--root=...]"); return 1; }
        ensure_manifest_dir();
        const char *name = argv[2];
        pakman_pkg_t pkgs[MAX_PKGS];
        int cnt = load_manifest(pkgs, MAX_PKGS);
        int found = -1;
        for (int i=0;i<cnt;i++) {
            if (strcmp(pkgs[i].name, name) == 0) { found = i; break; }
        }
        if (found < 0) { write_line("uninstall: not installed"); return 1; }

        rm_rf(pkgs[found].install_path);
        // remove from list
        for (int i=found;i<cnt-1;i++) pkgs[i] = pkgs[i+1];
        cnt--;
        save_manifest(pkgs, cnt);
        puts_raw("Uninstalled: "); write_line(name);
        return 0;
    }

    if (strcmp(cmd, "createpakfile") == 0) {
        if (argc < 4) { write_line("pakman createpakfile <folder> <out.pak>"); return 1; }
        if (pakzip_create_from_dir(argv[2], argv[3]) != 0) {
            write_line("createpakfile: failed");
            return 1;
        }
        write_line("createpakfile: ok");
        return 0;
    }

    print_usage();
    return 1;
}
