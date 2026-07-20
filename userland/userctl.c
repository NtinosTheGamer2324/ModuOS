// userctl_service.c
#include "libc.h"
#include "errno.h"
#include "string.h"
#include <stdint.h>
#include "sha256.h"

#define USER_BASE_DIR "/ModuOS/System64/Auth/usr"
#define MAX_USERS 256

typedef enum {
    CMD_GETUSERNAME,
    CMD_LOGIN,
    CMD_SETPASSWORD,
    CMD_CREATEUSER,
    CMD_DELETEUSER,
    CMD_WHOAMI,
} uctl_cmd_t;

typedef struct {
    uctl_cmd_t cmd;
    uint64_t intarg;
    uint32_t caller_pid;   /* client fills this with its own getpid() */
    char arg1[32];
    char arg2[32];
    char arg3[32];
} uctl_packet_t;

typedef struct {
    char type1ret[32];
    uint64_t type2ret;
    uint64_t callerpid;
} uctl_ret_t;

typedef struct {
    char username[32];
    uint64_t uid;
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
    char value[64];
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

static void generate_salt_hex(char *salt_hex, size_t salt_hex_sz) {
    if (!salt_hex || salt_hex_sz < 33) return;
    uint8_t salt_bytes[16];

    for (int i = 0; i < 2; i++) {
        uint64_t rand_val;
        int attempts = 0;
        while (!rdrand64(&rand_val) && attempts++ < 10);
        if (attempts >= 10)
            rand_val = (uint64_t)__builtin_ia32_rdtsc();
        memcpy(salt_bytes + i * 8, &rand_val, 8);
    }

    bytes_to_hex(salt_bytes, 16, salt_hex);
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

static int user_exists(const char *username) {
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
    const char *salt      = scf_get(f, "user", "salt");
    const char *hash      = scf_get(f, "user", "password");
    const char *groups    = scf_get(f, "user", "groups");

    if (!username || !uid_s || !salt || !hash) return -1;

    strlcpy(rec->username, username, sizeof(rec->username));
    rec->uid = (uint64_t)atoi(uid_s);
    strlcpy(rec->salt_hex, salt, sizeof(rec->salt_hex));
    strlcpy(rec->hash_hex, hash, sizeof(rec->hash_hex));
    strlcpy(rec->groups, groups ? groups : "", sizeof(rec->groups));
    return 0;
}

static int find_user(const char *username, user_record_t *rec) {
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

    uint64_t max_uid = 0;
    for (int i = 0; i < n; i++) {
        user_record_t r;
        if (find_user(names + i * 32, &r) == 0 && r.uid > max_uid) max_uid = r.uid;
    }
    free(names);
    return max_uid + 1;
}

static int create_user(const char *username, const char *password, uint64_t *out_uid) {
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

/* ---------------- identity helpers ---------------- */

/* getuid()/geteuid() inside this callback may not reflect the real caller
 * (see userfs_invoke_owner_callback's CR3 swap) — ask the kernel about the
 * caller's pid directly instead. Also requires userctl_service itself to
 * run as uid 0 / KERNEL_UID, since SYS_GETUID_OF_OTHER_PROCESS gates on
 * self's privilege, not the target's.
 *
 * NOTE: caller_pid here comes from packet->caller_pid, which the CLIENT
 * fills in with its own getpid() — it is NOT verified by the kernel and
 * a malicious client could lie about it. Fine for now; revisit if this
 * ever needs to be trustworthy against an adversarial client. */
static int caller_is_root(uint32_t caller_pid) {
    return getuid_of_other_process(caller_pid) == 0;
}

/* ---------------- invoke handler ---------------- */

static ssize_t uctl_invoke(void* ctx, const void* in_buf, size_t in_size,
                            void* out_buf, size_t out_size) {
    (void)ctx;
    if (in_size != sizeof(uctl_packet_t)) return -EINVAL;

    const uctl_packet_t* packet = (const uctl_packet_t*)in_buf;
    uint32_t caller_pid = packet->caller_pid;

    switch (packet->cmd) {

        case CMD_GETUSERNAME: {
            uctl_ret_t *ret = (uctl_ret_t*)out_buf;
            if (out_size < sizeof(uctl_ret_t)) return -EINVAL;
            ret->callerpid = caller_pid;

            user_record_t r;
            int rc = find_user_by_uid(packet->intarg, &r);
            if (rc != 0) return rc;

            strlcpy(ret->type1ret, r.username, sizeof(ret->type1ret));
            return sizeof(uctl_ret_t);
        }

        case CMD_LOGIN: {
            uctl_ret_t *ret = (uctl_ret_t*)out_buf;
            if (out_size < sizeof(uctl_ret_t)) return -EINVAL;
            ret->callerpid = caller_pid;

            user_record_t r;
            if (find_user(packet->arg1, &r) != 0) return -ENOENT;

            char hash_hex[65];
            hash_password(r.salt_hex, packet->arg2, hash_hex);
            if (strcmp(hash_hex, r.hash_hex) != 0) return -EACCES;

            /* setuid() only changes the caller of setuid()'s own uid —
             * which here would be this service, not the client. caller_pid
             * comes from the client's own packet->caller_pid field. */
            if (setuid_of_other_process((int)r.uid, caller_pid) != 0) return -EPERM;

            ret->type2ret = r.uid;
            strlcpy(ret->type1ret, r.groups, sizeof(ret->type1ret));
            return sizeof(uctl_ret_t);
        }

        case CMD_SETPASSWORD: {
            if (packet->arg1[0] == 0 || packet->arg3[0] == 0) return -EINVAL;
            int rc = set_password(packet->arg1, packet->arg2, packet->arg3, caller_is_root(caller_pid));
            return rc == 0 ? 0 : rc;
        }

        case CMD_CREATEUSER: {
            uctl_ret_t *ret = (uctl_ret_t*)out_buf;
            if (out_size < sizeof(uctl_ret_t)) return -EINVAL;
            ret->callerpid = caller_pid;
            if (packet->arg1[0] == 0 || packet->arg2[0] == 0) return -EINVAL;

            uint64_t uid;
            int rc = create_user(packet->arg1, packet->arg2, &uid);
            if (rc != 0) return rc;

            ret->type2ret = uid;
            return sizeof(uctl_ret_t);
        }

        case CMD_DELETEUSER: {
            if (!caller_is_root(caller_pid)) return -EACCES;
            int rc = delete_user(packet->arg1);
            return rc == 0 ? 0 : rc;
        }

        case CMD_WHOAMI: {
            uctl_ret_t *ret = (uctl_ret_t*)out_buf;
            if (out_size < sizeof(uctl_ret_t)) return -EINVAL;
            ret->callerpid = caller_pid;

            int caller_uid = getuid_of_other_process(caller_pid);

            snprintf(ret->type1ret, sizeof(ret->type1ret), "pid=%u uid=%d", caller_pid, caller_uid);
            ret->type2ret = (uint64_t)caller_pid;
            return sizeof(uctl_ret_t);
        }

        default:
            return -ENOSYS;
    }
}

int md_main(long argc, char** argv) {
    (void)argc; (void)argv;

    printf("userctl service starting...\n");

    /* USER_BASE_DIR's parent (/ModuOS/System64/Auth) must already exist;
     * mkdir here is not recursive. Failure is fine if it already exists,
     * or if the filesystem is read-only and it was created at install time. */
    mkdir(USER_BASE_DIR);

    userfs_user_ops_t ops = {0};
    ops.invoke = uctl_invoke;

    userfs_user_node_t node = {0};
    node.path     = "userctl";
    node.owner_id = "userctl_service";
    node.perms    = USERFS_PERM_READ_WRITE | USERFS_PERM_INVOKE;
    node.ops      = ops;
    node.ctx      = NULL;

    int ret = userfs_register(&node);
    if (ret != 0) {
        printf("Failed to register userctl service: %d\n", ret);
        return 1;
    }

    printf("userctl service registered at $/user/userctl\n");

    while (1) {
        yield();
    }

    return 0;
}