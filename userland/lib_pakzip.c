#define MINIZ_HEADER_FILE_ONLY
#define MINIZ_NO_STDIO
#define MINIZ_NO_TIME
#include "thirdparty/miniz/miniz.c"

#include "lib_pakzip.h"
#include "libc.h"
#include "string.h"

static int ensure_dir_recursive(const char *path) {
    // naive: try mkdir on every prefix
    char tmp[256];
    int n = 0;
    for (const char *p = path; *p && n < (int)sizeof(tmp)-1; p++) {
        tmp[n++] = *p;
        tmp[n] = 0;
        if (*p == '/') {
            if (n > 1) mkdir(tmp);
        }
    }
    mkdir(tmp);
    return 0;
}

static void path_join(const char *a, const char *b, char *out, int cap) {
    int pos = 0;
    if (!a) a = "";
    if (!b) b = "";
    for (int i=0; a[i] && pos<cap-1; i++) out[pos++] = a[i];
    if (pos && out[pos-1] != '/' && pos<cap-1) out[pos++] = '/';
    for (int i=0; b[i] && pos<cap-1; i++) out[pos++] = b[i];
    out[pos]=0;
}

int pakzip_extract(const char *zip_path, const char *dest_dir) {
    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));

    // Read zip into memory (miniz stdio disabled)
    int zfd = open(zip_path, 0, 0);
    if (zfd < 0) return -1;
    // Determine size by seeking
    int zsz = (int)lseek(zfd, 0, 2);
    lseek(zfd, 0, 0);
    if (zsz <= 0) { close(zfd); return -1; }
    void *zbuf = malloc((size_t)zsz);
    if (!zbuf) { close(zfd); return -1; }
    int off = 0;
    while (off < zsz) {
        int r = read(zfd, (char*)zbuf + off, zsz - off);
        if (r <= 0) break;
        off += r;
    }
    close(zfd);
    if (off != zsz) { free(zbuf); return -1; }

    if (!mz_zip_reader_init_mem(&zip, zbuf, (size_t)zsz, 0)) {
        free(zbuf);
        return -1;
    }

    int file_count = (int)mz_zip_reader_get_num_files(&zip);
    for (int i = 0; i < file_count; i++) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st)) {
            mz_zip_reader_end(&zip);
            return -1;
        }

        const char *name = st.m_filename;
        if (!name || !name[0]) continue;

        char outpath[512];
        path_join(dest_dir, name, outpath, sizeof(outpath));

        if (mz_zip_reader_is_file_a_directory(&zip, i)) {
            ensure_dir_recursive(outpath);
            continue;
        }

        // Ensure parent dir
        {
            char parent[512];
            strncpy(parent, outpath, sizeof(parent)-1);
            parent[sizeof(parent)-1] = 0;
            for (int j = (int)strlen(parent) - 1; j >= 0; j--) {
                if (parent[j] == '/') { parent[j] = 0; break; }
            }
            if (parent[0]) ensure_dir_recursive(parent);
        }

        size_t uncomp_size = 0;
        void *data = mz_zip_reader_extract_to_heap(&zip, i, &uncomp_size, 0);
        if (!data) {
            mz_zip_reader_end(&zip);
            return -1;
        }

        int fd = open(outpath, 0x2 | 0x40 | 0x200, 0); // O_RDWR|O_CREAT|O_TRUNC
        if (fd < 0) {
            mz_free(data);
            mz_zip_reader_end(&zip);
            return -1;
        }

        size_t off = 0;
        while (off < uncomp_size) {
            int w = write(fd, (const char*)data + off, (int)(uncomp_size - off));
            if (w <= 0) break;
            off += (size_t)w;
        }
        close(fd);
        mz_free(data);

        if (off != uncomp_size) {
            mz_zip_reader_end(&zip);
            return -1;
        }
    }

    mz_zip_reader_end(&zip);
    free(zbuf);
    return 0;
}

static int add_dir_recursive(mz_zip_archive *zip, const char *root, const char *rel) {
    char dirpath[512];
    if (rel && rel[0]) path_join(root, rel, dirpath, sizeof(dirpath));
    else strncpy(dirpath, root, sizeof(dirpath)-1);
    dirpath[sizeof(dirpath)-1] = 0;

    int dfd = opendir(dirpath);
    if (dfd < 0) return -1;

    while (1) {
        char name[260];
        int is_dir = 0;
        unsigned int sz = 0;
        int r = readdir(dfd, name, sizeof(name), &is_dir, &sz);
        if (r == 0) break;
        if (r < 0) { closedir(dfd); return -1; }

        // skip . and ..
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;

        char child_rel[512];
        if (rel && rel[0]) {
            path_join(rel, name, child_rel, sizeof(child_rel));
        } else {
            strncpy(child_rel, name, sizeof(child_rel)-1);
            child_rel[sizeof(child_rel)-1] = 0;
        }

        if (is_dir) {
            // add directory entry
            char slashname[520];
            strncpy(slashname, child_rel, sizeof(slashname)-2);
            slashname[sizeof(slashname)-2] = 0;
            int ln = strlen(slashname);
            if (ln == 0 || slashname[ln-1] != '/') {
                slashname[ln] = '/'; slashname[ln+1] = 0;
            }
            mz_zip_writer_add_mem(zip, slashname, "", 0, MZ_DEFAULT_LEVEL);
            if (add_dir_recursive(zip, root, child_rel) != 0) { closedir(dfd); return -1; }
        } else {
            char filepath[512];
            path_join(root, child_rel, filepath, sizeof(filepath));

            // read file
            int fd = open(filepath, 0, 0);
            if (fd < 0) { closedir(dfd); return -1; }
            char *buf = (char*)malloc(sz + 1);
            if (!buf) { close(fd); closedir(dfd); return -1; }
            unsigned int off = 0;
            while (off < sz) {
                int rr = read(fd, buf + off, (int)(sz - off));
                if (rr <= 0) break;
                off += (unsigned int)rr;
            }
            close(fd);
            if (off != sz) { free(buf); closedir(dfd); return -1; }

            // add file (we allow compression)
            if (!mz_zip_writer_add_mem(zip, child_rel, buf, sz, MZ_DEFAULT_LEVEL)) {
                free(buf);
                closedir(dfd);
                return -1;
            }
            free(buf);
        }
    }

    closedir(dfd);
    return 0;
}

int pakzip_create_from_dir(const char *src_dir, const char *out_zip_path) {
    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));

    // Build zip in memory then write out (miniz stdio disabled)
    void *out_buf = NULL;
    size_t out_size = 0;
    if (!mz_zip_writer_init_heap(&zip, 0, 0)) {
        return -1;
    }

    int rc = add_dir_recursive(&zip, src_dir, "");
    if (rc != 0) {
        mz_zip_writer_end(&zip);
        return -1;
    }

    if (!mz_zip_writer_finalize_archive(&zip)) {
        mz_zip_writer_end(&zip);
        return -1;
    }

    if (!mz_zip_writer_finalize_heap_archive(&zip, &out_buf, &out_size)) {
        mz_zip_writer_end(&zip);
        return -1;
    }

    mz_zip_writer_end(&zip);

    int fd = open(out_zip_path, 0x2 | 0x40 | 0x200, 0); // O_RDWR|O_CREAT|O_TRUNC
    if (fd < 0) { mz_free(out_buf); return -1; }
    size_t woff = 0;
    while (woff < out_size) {
        int w = write(fd, (const char*)out_buf + woff, (int)(out_size - woff));
        if (w <= 0) break;
        woff += (size_t)w;
    }
    close(fd);
    mz_free(out_buf);
    return (woff == out_size) ? 0 : -1;
}
