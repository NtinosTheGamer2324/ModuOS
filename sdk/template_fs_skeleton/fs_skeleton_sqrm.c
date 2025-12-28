#include "../sqrm_sdk.h"

/*
 * External filesystem skeleton.
 * Demonstrates how to register an FS driver via SQRM.
 *
 * This does NOT implement a real filesystem. All operations fail.
 */

SQRM_DEFINE_MODULE(SQRM_TYPE_FS, "fs_skel");

static const uint16_t COM1_PORT = 0x3F8;

static int sk_probe(int vdrive_id, uint32_t partition_lba) {
    (void)vdrive_id; (void)partition_lba;
    return 0; /* not recognized */
}

static int sk_mount(int vdrive_id, uint32_t partition_lba, fs_mount_t *mount) {
    (void)vdrive_id; (void)partition_lba; (void)mount;
    return -1;
}

static int sk_read_file(fs_mount_t *m, const char *path, void *buf, size_t buf_sz, size_t *out_read) {
    (void)m; (void)path; (void)buf; (void)buf_sz;
    if (out_read) *out_read = 0;
    return -1;
}

static int sk_stat(fs_mount_t *m, const char *path, fs_file_info_t *info) {
    (void)m; (void)path; (void)info;
    return -1;
}

static int sk_file_exists(fs_mount_t *m, const char *path) { (void)m; (void)path; return 0; }
static int sk_dir_exists(fs_mount_t *m, const char *path) { (void)m; (void)path; return 0; }
static int sk_list_dir(fs_mount_t *m, const char *path) { (void)m; (void)path; return -1; }
static fs_dir_t* sk_opendir(fs_mount_t *m, const char *path) { (void)m; (void)path; return NULL; }
static int sk_readdir(fs_dir_t *d, fs_dirent_t *e) { (void)d; (void)e; return 0; }
static void sk_closedir(fs_dir_t *d) { (void)d; }

static const fs_ext_driver_ops_t sk_ops = {
    .probe = sk_probe,
    .mount = sk_mount,
    .unmount = NULL,
    .mkfs = NULL,
    .read_file = sk_read_file,
    .stat = sk_stat,
    .file_exists = sk_file_exists,
    .directory_exists = sk_dir_exists,
    .list_directory = sk_list_dir,
    .opendir = sk_opendir,
    .readdir = sk_readdir,
    .closedir = sk_closedir,
};

int sqrm_module_init(const sqrm_kernel_api_t *api) {
    if (!api || api->abi_version != SQRM_ABI_VERSION) return -1;

    if (api->com_write_string) {
        api->com_write_string(COM1_PORT, "[fs_skel] init\n");
    }

    if (!api->fs_register_driver) {
        if (api->com_write_string) api->com_write_string(COM1_PORT, "[fs_skel] fs_register_driver not available\n");
        return -2;
    }

    return api->fs_register_driver("fs_skel", &sk_ops);
}
