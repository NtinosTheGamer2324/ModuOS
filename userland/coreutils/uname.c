// uname.c — print system information
// Reads from $/dev/md64api/sysinfo like zsfetch.
// pcname is read from /ModuOS/System64/pcname.txt like zenith.

#include "libc.h"

static const char *read_pcname(char *buf, size_t bufsz)
{
    const char *path = "/ModuOS/System64/pcname.txt";
    fs_file_info_t fi;
    if (stat(path, &fi) < 0) return NULL;
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) return NULL;
    ssize_t n = read(fd, buf, bufsz - 1);
    close(fd);
    if (n < 0) return NULL;
    buf[n] = 0;
    return buf;
}

static void usage(const char *argv0)
{
    printf("Usage: %s [OPTION]...\n", argv0);
    puts("  -a  print all information");
    puts("  -s  kernel name (default)");
    puts("  -n  network node / hostname");
    puts("  -r  kernel release");
    puts("  -v  kernel version / vendor");
    puts("  -m  machine architecture");
    puts("  -o  operating system name");
}

int md_main(long argc, char **argv)
{
    int opt_all  = 0;
    int opt_s    = 0; // kernel name
    int opt_n    = 0; // nodename
    int opt_r    = 0; // kernel release
    int opt_v    = 0; // kernel version
    int opt_m    = 0; // machine
    int opt_o    = 0; // OS name

    if (argc < 2) {
        opt_s = 1;
    }

    for (int i = 1; i < (int)argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--help") || !strcmp(a, "-h")) { usage(argv[0]); return 0; }
        if (!strcmp(a, "-a")) { opt_all = 1; continue; }
        // Flags may be combined: -sn, -rm, etc.
        if (a[0] == '-') {
            for (int j = 1; a[j]; j++) {
                switch (a[j]) {
                    case 's': opt_s = 1; break;
                    case 'n': opt_n = 1; break;
                    case 'r': opt_r = 1; break;
                    case 'v': opt_v = 1; break;
                    case 'm': opt_m = 1; break;
                    case 'o': opt_o = 1; break;
                    default:
                        printf("uname: unknown option: -%c\n", a[j]);
                        return 1;
                }
            }
        }
    }

    md64api_sysinfo_data_u *info =
        (md64api_sysinfo_data_u *)malloc(sizeof(md64api_sysinfo_data_u));
    if (!info) { puts("uname: out of memory"); return 1; }
    memset(info, 0, sizeof(md64api_sysinfo_data_u));

    int fd = open("$/dev/md64api/sysinfo", O_RDONLY, 0);
    if (fd < 0) { puts("uname: cannot open sysinfo"); free(info); return 1; }
    read(fd, info, sizeof(md64api_sysinfo_data_u));
    close(fd);

    char pcname_buf[128];
    const char *pcname = read_pcname(pcname_buf, sizeof(pcname_buf));
    if (!pcname) pcname = "unknown";

    // Build version string: "Version <n> - <vendor>"
    char ver_buf[128];
    snprintf(ver_buf, sizeof(ver_buf), "Version %d - %s",
             info->KernelVersion,
             info->KernelVendor && info->KernelVendor[0]
                 ? info->KernelVendor : "Unknown");

    // Build release string from SystemVersion
    char rel_buf[32];
    snprintf(rel_buf, sizeof(rel_buf), "%d", info->SystemVersion);

    int first = 1;
#define EMIT(str) do { if (!first) putc(' '); puts_raw(str); first = 0; } while(0)

    if (opt_all || opt_s) EMIT(info->os_name && info->os_name[0] ? info->os_name : "ModuOS");
    if (opt_all || opt_n) EMIT(pcname);
    if (opt_all || opt_r) EMIT(rel_buf);
    if (opt_all || opt_v) EMIT(ver_buf);
    if (opt_all || opt_m) EMIT(info->os_arch && info->os_arch[0] ? info->os_arch : "AMD64");
    if (opt_all || opt_o) EMIT(info->os_name && info->os_name[0] ? info->os_name : "ModuOS");

#undef EMIT

    putc('\n');
    free(info);
    return 0;
}