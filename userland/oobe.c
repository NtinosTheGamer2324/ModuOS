#include "libc.h"
#include "string.h"

/* --- SAFE STRING HELPERS --- */
static void copy_str(char *dst, size_t dst_sz, const char *src) {
    if (!dst || !src || dst_sz == 0) return;
    size_t i = 0;
    for (; i + 1 < dst_sz && src[i]; i++) dst[i] = src[i];
    dst[i] = 0;
}

static void set_str(char *dst, size_t dst_sz, const char *src) {
    copy_str(dst, dst_sz, src);
}

static void append_str(char *dst, size_t dst_sz, const char *src) {
    if (!dst || !src || dst_sz == 0) return;
    size_t len = strlen(dst);
    if (len >= dst_sz - 1) return;
    size_t rem = dst_sz - 1 - len;
    size_t s_len = strlen(src);
    size_t cp = (s_len > rem) ? rem : s_len;
    memcpy(dst + len, src, cp);
    dst[len + cp] = 0;
}

/* --- SYSTEM PATHS --- */
#define OOBE_FLAG_PATH "/ModuOS/System64/OOBE/oobe-bt"
#define OOBE_ASSET_BASE "/ModuOS/System64/OOBE/ASSETS/sqrm"
#define OOBE_MD_PATH "/ModuOS/System64/md"

/* --- FILE OPERATIONS --- */
static int copy_file(const char *src, const char *dst) {
    int sfd = open(src, O_RDONLY, 0);
    if (sfd < 0) return -1;
    int dfd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0);
    if (dfd < 0) { close(sfd); return -1; }

    char buf[4096];
    int r;
    while ((r = read(sfd, buf, sizeof(buf))) > 0) {
        if (write(dfd, buf, (size_t)r) != r) {
            close(sfd); close(dfd); return -1;
        }
    }
    close(sfd); close(dfd);
    return 0;
}

static int remove_tree(const char *path) {
    int d = opendir(path);
    if (d < 0) return -1;
    char name[260];
    int is_dir = 0;
    uint32_t size = 0;
    while (readdir(d, name, sizeof(name), &is_dir, &size) == 0) {
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
        char full[512];
        set_str(full, sizeof(full), path);
        append_str(full, sizeof(full), "/");
        append_str(full, sizeof(full), name);
        if (is_dir) remove_tree(full);
        else unlink(full);
    }
    closedir(d);
    return rmdir(path);
}

/* --- THE MISSING LINK: CHOOSE OPTION --- */
static int choose_option(const char *title, const char *dir, char *out_name, size_t out_sz) {
    printf("\n== %s ==\nScanning: %s\n", title, dir);
    int d = opendir(dir);
    if (d < 0) { printf("No options found.\n"); return -1; }

    char names[16][128];
    int count = 0;
    char name[260];
    int is_dir = 0;
    uint32_t size = 0;

    while (readdir(d, name, sizeof(name), &is_dir, &size) == 0 && count < 16) {
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0 || !is_dir) continue;
        set_str(names[count], sizeof(names[count]), name);
        printf("  %d) %s\n", count + 1, name);
        count++;
    }
    closedir(d);

    if (count == 0) return -1;
    printf("Select [1-%d]: ", count);
    char *sel = input();
    int idx = atoi(sel) - 1;
    if (idx < 0 || idx >= count) { printf("Invalid.\n"); return -1; }

    set_str(out_name, out_sz, names[idx]);
    return 0;
}

static int install_driver_category(const char *cat, const char *driver_name) {
    if (!driver_name || !*driver_name) return -1;
    char src_p[512];
    set_str(src_p, sizeof(src_p), OOBE_ASSET_BASE);
    append_str(src_p, sizeof(src_p), "/");
    append_str(src_p, sizeof(src_p), cat);
    append_str(src_p, sizeof(src_p), "/");
    append_str(src_p, sizeof(src_p), driver_name);

    int d = opendir(src_p);
    if (d < 0) return -1;
    char name[260];
    int is_dir = 0;
    uint32_t fsize = 0;
    while (readdir(d, name, sizeof(name), &is_dir, &fsize) == 0) {
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0 || is_dir) continue;
        char s_file[512], d_file[512];
        set_str(s_file, sizeof(s_file), src_p);
        append_str(s_file, sizeof(s_file), "/");
        append_str(s_file, sizeof(s_file), name);
        set_str(d_file, sizeof(d_file), OOBE_MD_PATH);
        append_str(d_file, sizeof(d_file), "/");
        append_str(d_file, sizeof(d_file), name);
        copy_file(s_file, d_file);
    }
    closedir(d);
    return 0;
}

/* --- MAIN --- */
int md_main(long argc, char **argv) {
    (void)argc; (void)argv;
    int fd = open(OOBE_FLAG_PATH, O_RDONLY, 0);
    if (fd < 0) { printf("OOBE already completed.\n"); return 0; }
    close(fd);

    char gpu[128] = {0}, net[128] = {0}, fs[128] = {0};
    if (choose_option("GPU Driver", OOBE_ASSET_BASE "/gpu", gpu, sizeof(gpu))) return 1;
    if (choose_option("Network Driver", OOBE_ASSET_BASE "/net", net, sizeof(net))) return 1;
    if (choose_option("Filesystem Driver", OOBE_ASSET_BASE "/fs", fs, sizeof(fs))) return 1;

    printf("\nApplying selection...\n");
    remove_tree(OOBE_MD_PATH);
    mkdir(OOBE_MD_PATH);

    install_driver_category("gpu", gpu);
    install_driver_category("net", net);
    install_driver_category("fs", fs);

    unlink(OOBE_FLAG_PATH);
    printf("OOBE complete.\n");
    return 0;
}