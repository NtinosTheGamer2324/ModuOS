// userctl_service.c - account database and authentication service (SHM IPC)
#include "libc/libc.h"
#include "errno.h"
#include "libc/string.h"
#include <stdint.h>
#include "sha256.h"
#include "userctl_ipc.h"

#define USER_BASE_DIR "/ModuOS/System64/Auth/usr"
#define MAX_USERS 256

typedef struct {
    char username[32];
    uint64_t uid;
    uint64_t gid;
    char salt_hex[33];
    char hash_hex[65];
    char groups[64];
} user_record_t;

/* ---------------- growable buffer ---------------- */

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} dbuf_t;

static void dbuf_init(dbuf_t *b) {
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

static int dbuf_append(dbuf_t *b, const char *s, size_t n) {
    if (b->len + n + 1 > b->cap) {
        size_t ncap = (b->cap == 0) ? 256 : b->cap * 2;
        while (ncap < b->len + n + 1) ncap *= 2;
        char *nd = realloc(b->data, ncap);
        if (!nd) return -1;
        b->data = nd;
        b->cap = ncap;
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = 0;
    return 0;
}

/* ---------------- .scf parser (INI-style, [section] + key=value) ---------------- */

#define SCF_MAX_SECTIONS 4
#define SCF_MAX_KV       16

typedef struct {
    char key[32];
    char value[96]; /* must hold a 64-char hex hash plus NUL */
} scf_kv_t;

typedef struct {
    char name[32];
    scf_kv_t kv[SCF_MAX_KV];
    int kv_count;
} scf_section_t;

typedef struct {
    scf_section_t sections[SCF_MAX_SECTIONS];
    int section_count;
} scf_file_t;

static char *scf_trim(char *s) {
    while (*s == ' ' || *s == '\t' || *s == '\r') s++;
    char *end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r')) *--end = 0;
    return s;
}

static scf_section_t *scf_find_section(scf_file_t *f, const char *name) {
    for (int i = 0; i < f->section_count; i++)
        if (strcmp(f->sections[i].name, name) == 0) return &f->sections[i];
    return NULL;
}

static scf_section_t *scf_ensure_section(scf_file_t *f, const char *name) {
    scf_section_t *s = scf_find_section(f, name);
    if (s) return s;
    if (f->section_count >= SCF_MAX_SECTIONS) return NULL;
    s = &f->sections[f->section_count++];
    memset(s, 0, sizeof(*s));
    strlcpy(s->name, name, sizeof(s->name));
    return s;
}

static void scf_parse(char *buf, scf_file_t *out) {
    memset(out, 0, sizeof(*out));
    scf_section_t *cur = NULL;

    char *line = strtok(buf, "\n");
    while (line) {
        char *l = scf_trim(line);
        if (*l == 0 || *l == '#') { line = strtok(NULL, "\n"); continue; }

        if (l[0] == '[') {
            char *end = strchr(l, ']');
            if (end) *end = 0;
            cur = scf_ensure_section(out, l + 1);
        } else if (cur) {
            char *eq = strchr(l, '=');
            if (eq && cur->kv_count < SCF_MAX_KV) {
                *eq = 0;
                char *key = scf_trim(l);
                char *val = scf_trim(eq + 1);
                scf_kv_t *kv = &cur->kv[cur->kv_count++];
                strlcpy(kv->key, key, sizeof(kv->key));
                strlcpy(kv->value, val, sizeof(kv->value));
            }
        }
        line = strtok(NULL, "\n");
    }
}

static const char *scf_get(scf_file_t *f, const char *section, const char *key) {
    scf_section_t *s = scf_find_section(f, section);
    if (!s) return NULL;
    for (int i = 0; i < s->kv_count; i++)
        if (strcmp(s->kv[i].key, key) == 0) return s->kv[i].value;
    return NULL;
}

static int scf_set(scf_file_t *f, const char *section, const char *key, const char *value) {
    scf_section_t *s = scf_ensure_section(f, section);
    if (!s) return -1;
    for (int i = 0; i < s->kv_count; i++) {
        if (strcmp(s->kv[i].key, key) == 0) {
            strlcpy(s->kv[i].value, value, sizeof(s->kv[i].value));
            return 0;
        }
    }
    if (s->kv_count >= SCF_MAX_KV) return -1;
    scf_kv_t *kv = &s->kv[s->kv_count++];
    strlcpy(kv->key, key, sizeof(kv->key));
    strlcpy(kv->value, value, sizeof(kv->value));
    return 0;
}

static void scf_serialize(scf_file_t *f, dbuf_t *out) {
    for (int i = 0; i < f->section_count; i++) {
        scf_section_t *s = &f->sections[i];
        char line[96];
        int n = snprintf(line, sizeof(line), "[%s]\n", s->name);
        dbuf_append(out, line, n);
        for (int j = 0; j < s->kv_count; j++) {
            n = snprintf(line, sizeof(line), "%s=%s\n", s->kv[j].key, s->kv[j].value);
            dbuf_append(out, line, n);
        }
        dbuf_append(out, "\n", 1);
    }
}

/* ---------------- rng / hashing ---------------- */

static int rdrand64(uint64_t *result) {
    unsigned char ok;
    __asm__ __volatile__(
        "rdrand %0; setc %1"
        : "=r" (*result), "=qm" (ok)
    );
    return ok;
}

static void bytes_to_hex(const uint8_t *bytes, size_t n, char *out_hex) {
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out_hex[i*2]     = digits[(bytes[i] >> 4) & 0xF];
        out_hex[i*2 + 1] = digits[bytes[i] & 0xF];
    }
    out_hex[n*2] = 0;
}

#define URANDOM_PATH "$/dev/md64api/urandom"

static int random_bytes(void *buf, size_t len) {
    int fd = open(URANDOM_PATH, O_RDONLY, 0);
    if (fd < 0) return -1;

    uint8_t *p = buf;
    size_t done = 0;

    while (done < len) {
        ssize_t n = read(fd, p + done, len - done);

        if (n <= 0) {
            close(fd);
            memset(buf, 0, len);
            return -1;
        }

        done += (size_t)n;
    }

    close(fd);
    return 0;
}

static int generate_salt_hex(char *salt_hex, size_t salt_hex_sz) {
    if (!salt_hex || salt_hex_sz < 33)
        return -1;

    uint8_t salt_bytes[16];

    if (random_bytes(salt_bytes, sizeof(salt_bytes)) != 0)
        return -1;

    bytes_to_hex(salt_bytes, sizeof(salt_bytes), salt_hex);
    memset(salt_bytes, 0, sizeof(salt_bytes));

    return 0;
}

/* password hash = SHA256(salt_hex || password), stored as 64 hex chars */
static void hash_password(const char *salt_hex, const char *password, char out_hex[65]) {
    sha256_ctx ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, salt_hex, strlen(salt_hex));
    sha256_update(&ctx, password, strlen(password));

    uint8_t digest[32];
    sha256_final(&ctx, digest);
    sha256_to_hex(digest, out_hex);
}

/* ---------------- raw file I/O ---------------- */

static char *read_entire_file(const char *path, size_t *out_len) {
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) return NULL;

    size_t cap = 4096, len = 0;
    char *buf = malloc(cap);
    if (!buf) { close(fd); return NULL; }

    for (;;) {
        if (len + 4096 > cap) {
            cap *= 2;
            char *nb = realloc(buf, cap);
            if (!nb) { free(buf); close(fd); return NULL; }
            buf = nb;
        }
        ssize_t n = read(fd, buf + len, 4096);
        if (n < 0) { free(buf); close(fd); return NULL; }
        if (n == 0) break;
        len += (size_t)n;
    }
    close(fd);

    char *nb = realloc(buf, len + 1);
    if (nb) buf = nb;
    buf[len] = 0;
    if (out_len) *out_len = len;
    return buf;
}

/* Returns 0 on success, -EROFS if the write itself fails. Syscalls here
 * don't reliably surface a distinct read-only errno, so any write failure
 * against an existing/creatable path is treated as "filesystem not writable"
 * for the caller's purposes.
 */
static int write_entire_file(const char *path, const char *buf, size_t len) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0);
    if (fd < 0) return -EROFS;

    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, buf + off, len - off);
        if (n <= 0) { close(fd); return -EROFS; }
        off += (size_t)n;
    }
    close(fd);
    return 0;
}

/* ---------------- per-user .scf storage ---------------- */

static void user_dir_path(const char *username, char *buf, size_t sz) {
    snprintf(buf, sz, "%s/%s", USER_BASE_DIR, username);
}

static void user_scf_path(const char *username, char *buf, size_t sz) {
    snprintf(buf, sz, "%s/%s/user.scf", USER_BASE_DIR, username);
}

/* Names become directory names under USER_BASE_DIR, so anything that could
 * traverse or confuse the path is rejected. */
static int username_valid(const char *s) {
    size_t n = strlen(s);
    if (n == 0 || n >= UCTL_NAME_MAX) return 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                 (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) return 0;
    }
    return 1;
}

static int user_exists(const char *username) {
    if (!username_valid(username)) return 0;
    char path[96];
    user_scf_path(username, path, sizeof(path));
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) return 0;
    close(fd);
    return 1;
}

static int read_user_scf(const char *username, scf_file_t *out) {
    char path[96];
    user_scf_path(username, path, sizeof(path));

    size_t len;
    char *buf = read_entire_file(path, &len);
    if (!buf) return -ENOENT;

    scf_parse(buf, out);
    free(buf);
    return 0;
}

static int write_user_scf(const char *username, scf_file_t *f) {
    char path[96];
    user_scf_path(username, path, sizeof(path));

    dbuf_t out;
    dbuf_init(&out);
    scf_serialize(f, &out);

    int rc = write_entire_file(path, out.data, out.len);
    free(out.data);
    return rc;
}

static int record_from_scf(scf_file_t *f, user_record_t *rec) {
    const char *username = scf_get(f, "user", "username");
    const char *uid_s     = scf_get(f, "user", "uid");
    const char *gid_s     = scf_get(f, "user", "gid");
    const char *salt      = scf_get(f, "user", "salt");
    const char *hash      = scf_get(f, "user", "password");
    const char *groups    = scf_get(f, "user", "groups");

    if (!username || !uid_s || !salt || !hash) return -1;

    strlcpy(rec->username, username, sizeof(rec->username));
    rec->uid = (uint64_t)atoi(uid_s);
    /* Accounts created before gid existed get a private group (gid == uid). */
    rec->gid = gid_s ? (uint64_t)atoi(gid_s) : rec->uid;
    strlcpy(rec->salt_hex, salt, sizeof(rec->salt_hex));
    strlcpy(rec->hash_hex, hash, sizeof(rec->hash_hex));
    strlcpy(rec->groups, groups ? groups : "", sizeof(rec->groups));
    return 0;
}

static int find_user(const char *username, user_record_t *rec) {
    if (!username_valid(username)) return -ENOENT;
    scf_file_t *f = malloc(sizeof(scf_file_t));
    if (!f) return -ENOENT;

    int rc = read_user_scf(username, f);
    if (rc == 0) rc = record_from_scf(f, rec);

    free(f);
    return rc == 0 ? 0 : -ENOENT;
}

/* Lists usernames (subdirectories of USER_BASE_DIR) into a malloc'd
 * buffer of `max` rows of 32 bytes each. Caller frees *out_names. */
static int list_usernames(char **out_names, int max) {
    char *buf = malloc((size_t)max * 32);
    if (!buf) { *out_names = NULL; return 0; }

    int fd = opendir(USER_BASE_DIR);
    if (fd < 0) { free(buf); *out_names = NULL; return 0; }

    int count = 0;
    char name[64];
    int is_dir;
    uint32_t size;
    while (count < max && readdir(fd, name, sizeof(name), &is_dir, &size) == 0) {
        if (!is_dir) continue;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
        strlcpy(buf + count * 32, name, 32);
        count++;
    }
    closedir(fd);

    *out_names = buf;
    return count;
}

static int find_user_by_uid(uint64_t uid, user_record_t *rec) {
    char *names;
    int n = list_usernames(&names, MAX_USERS);

    int found = -ENOENT;
    for (int i = 0; i < n; i++) {
        user_record_t r;
        if (find_user(names + i * 32, &r) == 0 && r.uid == uid) {
            *rec = r;
            found = 0;
            break;
        }
    }
    free(names);
    return found;
}

static uint64_t get_next_uid(void) {
    char *names;
    int n = list_usernames(&names, MAX_USERS);

    if (n == 0) { free(names); return 0; }

    uint64_t max_uid = 0;
    for (int i = 0; i < n; i++) {
        user_record_t r;
        if (find_user(names + i * 32, &r) == 0 && r.uid > max_uid) max_uid = r.uid;
    }
    free(names);
    return max_uid + 1;
}

static int create_user(const char *username, const char *password, uint64_t *out_uid) {
    if (!username_valid(username)) return -EINVAL;
    if (user_exists(username)) return -EEXIST;

    char dirpath[96];
    user_dir_path(username, dirpath, sizeof(dirpath));
    if (mkdir(dirpath) != 0) return -EROFS;

    char salt_hex[33];
    generate_salt_hex(salt_hex, sizeof(salt_hex));
    char hash_hex[65];
    hash_password(salt_hex, password, hash_hex);
    uint64_t uid = get_next_uid();

    char uid_s[24];
    itoa((int)uid, uid_s, 10);

    scf_file_t *f = malloc(sizeof(scf_file_t));
    if (!f) { rmdir(dirpath); return -ENOMEM; }
    memset(f, 0, sizeof(*f));
    scf_set(f, "user", "uid", uid_s);
    scf_set(f, "user", "gid", uid_s);
    scf_set(f, "user", "groups", "user:1;");
    scf_set(f, "user", "salt", salt_hex);
    scf_set(f, "user", "username", username);
    scf_set(f, "user", "password", hash_hex);
    scf_set(f, "properties", "needs_oobe", "true");

    int rc = write_user_scf(username, f);
    free(f);
    if (rc != 0) {
        rmdir(dirpath); /* roll back the empty directory we just made */
        return rc;
    }

    *out_uid = uid;
    return 0;
}

static int delete_user(const char *username) {
    if (!user_exists(username)) return -ENOENT;

    char scf_path[96], dir_path[96];
    user_scf_path(username, scf_path, sizeof(scf_path));
    user_dir_path(username, dir_path, sizeof(dir_path));

    if (unlink(scf_path) != 0) return -EROFS;
    rmdir(dir_path); /* best-effort */
    return 0;
}

static int set_password(const char *username, const char *old_pw, const char *new_pw, int is_root) {
    scf_file_t *f = malloc(sizeof(scf_file_t));
    if (!f) return -ENOMEM;
    if (read_user_scf(username, f) != 0) { free(f); return -ENOENT; }

    const char *salt = scf_get(f, "user", "salt");
    const char *hash = scf_get(f, "user", "password");
    if (!salt || !hash) { free(f); return -ENOENT; }

    if (!is_root) {
        char check[65];
        hash_password(salt, old_pw, check);
        if (strcmp(check, hash) != 0) { free(f); return -EACCES; }
    }

    char new_salt[33];
    generate_salt_hex(new_salt, sizeof(new_salt));
    char new_hash[65];
    hash_password(new_salt, new_pw, new_hash);

    scf_set(f, "user", "salt", new_salt);
    scf_set(f, "user", "password", new_hash);

    int rc = write_user_scf(username, f);
    free(f);
    return rc;
}

/* ---------------- authorization ---------------- */

static int ct_streq(const char *a, const char *b) {
    unsigned char diff = 0;
    size_t la = strlen(a), lb = strlen(b);
    diff |= (unsigned char)(la != lb);
    for (size_t i = 0; i < la && i < lb; i++) diff |= (unsigned char)(a[i] ^ b[i]);
    return diff == 0;
}

static int password_matches(const user_record_t *r, const char *password) {
    char hex[65];
    hash_password(r->salt_hex, password, hex);
    return ct_streq(hex, r->hash_hex);
}

/* The mailbox cannot identify callers, so privileged commands must carry
 * the credentials of a uid 0 account. */
static int admin_authenticated(const uctl_request_t *req) {
    user_record_t r;
    if (find_user(req->admin_user, &r) != 0) return 0;
    return r.uid == 0 && password_matches(&r, req->admin_pass);
}

static int any_user_exists(void) {
    char *names;
    int n = list_usernames(&names, 1);
    free(names);
    return n > 0;
}

/* ---------------- command dispatch ---------------- */

static int uctl_handle(const uctl_request_t *req, uctl_response_t *resp) {
    switch (req->cmd) {

        case UCTL_CMD_GETUSERNAME: {
            user_record_t r;
            if (find_user_by_uid(req->intarg, &r) != 0) return -ENOENT;
            strlcpy(resp->name, r.username, sizeof(resp->name));
            return 0;
        }

        case UCTL_CMD_LOGIN: {
            user_record_t r;
            /* Same error for unknown user and bad password. */
            if (find_user(req->arg_user, &r) != 0) return -EACCES;
            if (!password_matches(&r, req->arg_pass)) return -EACCES;
            resp->uid = r.uid;
            resp->gid = r.gid;
            strlcpy(resp->name, r.username, sizeof(resp->name));
            strlcpy(resp->groups, r.groups, sizeof(resp->groups));
            return 0;
        }

        case UCTL_CMD_SETPASSWORD: {
            if (req->arg_user[0] == 0 || req->arg_new[0] == 0) return -EINVAL;
            int as_admin = req->admin_user[0] != 0;
            if (as_admin && !admin_authenticated(req)) return -EACCES;
            return set_password(req->arg_user, req->arg_pass, req->arg_new, as_admin);
        }

        case UCTL_CMD_CREATEUSER: {
            if (req->arg_user[0] == 0 || req->arg_pass[0] == 0) return -EINVAL;
            if (any_user_exists() && !admin_authenticated(req)) return -EACCES;
            uint64_t uid;
            int rc = create_user(req->arg_user, req->arg_pass, &uid);
            if (rc != 0) return rc;
            resp->uid = uid;
            resp->gid = uid;
            return 0;
        }

        case UCTL_CMD_DELETEUSER: {
            if (!admin_authenticated(req)) return -EACCES;
            user_record_t r;
            if (find_user(req->arg_user, &r) != 0) return -ENOENT;
            if (r.uid == 0) return -EPERM; /* never delete the last way in */
            return delete_user(req->arg_user);
        }

        default:
            return -ENOSYS;
    }
}

int md_main(long argc, char **argv) {
    (void)argc; (void)argv;

    printf("userctl service starting...\n");

    /* USER_BASE_DIR's parent (/ModuOS/System64/Auth) must already exist;
     * mkdir here is not recursive. Failure is fine if it already exists,
     * or if the filesystem is read-only and it was created at install time. */
    mkdir(USER_BASE_DIR);

    /* Drop a stale segment from a previous run, then create ours exclusively:
     * clients trust whoever owns the name, and shm_unlink is owner-only. */
    shm_unlink(UCTL_IPC_SHM_NAME);
    int h = shm_open(UCTL_IPC_SHM_NAME, O_RDWR | SHM_O_CREAT | SHM_O_EXCL, 0666,
                     sizeof(uctl_shm_t));
    if (h < 0) {
        printf("userctl: shm_open(%s) failed (errno=%d)\n", UCTL_IPC_SHM_NAME, -h);
        return 1;
    }

    uctl_shm_t *shm = (uctl_shm_t *)mmap(NULL, sizeof(uctl_shm_t), PROT_R | PROT_W,
                                         MAP_SHARED, h);
    if (shm == MAP_FAILED) {
        printf("userctl: mmap of mailbox failed\n");
        return 1;
    }
    memset(shm, 0, sizeof(*shm));

    printf("userctl: mailbox '%s' ready\n", UCTL_IPC_SHM_NAME);

    uctl_request_t  req;
    uctl_response_t resp;
    for (;;) {
        if (uctl_get_state(shm) == UCTL_SLOT_REQUEST_PENDING) {
            /* Work on a private copy so the secrets leave shared memory
             * before the slow part (filesystem I/O) starts. */
            memcpy(&req, &shm->req, sizeof(req));
            uctl_scrub(&shm->req, sizeof(shm->req));
            req.arg_user[sizeof(req.arg_user) - 1] = 0;
            req.admin_user[sizeof(req.admin_user) - 1] = 0;
            req.arg_pass[sizeof(req.arg_pass) - 1] = 0;
            req.arg_new[sizeof(req.arg_new) - 1] = 0;
            req.admin_pass[sizeof(req.admin_pass) - 1] = 0;

            memset(&resp, 0, sizeof(resp));
            resp.status = uctl_handle(&req, &resp);
            uctl_scrub(&req, sizeof(req));

            memcpy(&shm->resp, &resp, sizeof(resp));
            uctl_set_state(shm, UCTL_SLOT_RESPONSE_READY);
        }
        yield();
    }

    return 0;
}