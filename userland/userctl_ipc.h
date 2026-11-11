// userctl_ipc.h - shared-memory IPC between userctl and its clients.
//
// userFS invoke() proved unreliable for this service, so requests travel
// through a single-slot SHM mailbox, the same scheme netd uses.
//
// The mailbox carries no trustworthy caller identity: any process can map
// the segment and write any field. Authorization therefore never depends on
// who the caller claims to be. Every privileged command carries proof of
// credentials (admin_user/admin_pass for a uid 0 account, or the target
// user's current password), and LOGIN only reports the uid/gid; the client
// applies them itself with setgid()/setuid(), so userctl never changes
// another process's credentials.
//
// Server (userctl):
//   shm_unlink + shm_open(UCTL_IPC_SHM_NAME, O_RDWR | SHM_O_CREAT | SHM_O_EXCL)
//   mmap once, then answer UCTL_SLOT_REQUEST_PENDING with RESPONSE_READY.
// Client:
//   uctl_attach() once, then uctl_call() per request.

#pragma once
#include "libc/libc.h"
#include "libc/string.h"

#define UCTL_IPC_SHM_NAME         "userctl_ipc"
#define UCTL_IPC_LOCK_TIMEOUT_MS  2000
#define UCTL_IPC_RESPONSE_WAIT_MS 5000

#define UCTL_NAME_MAX   32
#define UCTL_PASS_MAX   64
#define UCTL_GROUPS_MAX 64

typedef enum {
    UCTL_CMD_GETUSERNAME = 1,  /* intarg = uid                  -> name        */
    UCTL_CMD_LOGIN       = 2,  /* arg_user, arg_pass            -> uid, gid, groups */
    UCTL_CMD_SETPASSWORD = 3,  /* arg_user, arg_pass (old), arg_new
                                * or admin_* instead of the old password       */
    UCTL_CMD_CREATEUSER  = 4,  /* arg_user, arg_pass, admin_*   -> uid
                                * (admin_* not needed while no users exist)    */
    UCTL_CMD_DELETEUSER  = 5,  /* arg_user, admin_*                            */
} uctl_cmd_t;

typedef struct {
    int      cmd;
    uint64_t intarg;
    char     arg_user[UCTL_NAME_MAX];
    char     arg_pass[UCTL_PASS_MAX];
    char     arg_new[UCTL_PASS_MAX];
    char     admin_user[UCTL_NAME_MAX];
    char     admin_pass[UCTL_PASS_MAX];
} uctl_request_t;

typedef struct {
    int      status;             /* 0 on success, -errno otherwise */
    uint64_t uid;
    uint64_t gid;
    char     name[UCTL_NAME_MAX];
    char     groups[UCTL_GROUPS_MAX];
} uctl_response_t;

typedef enum {
    UCTL_SLOT_IDLE            = 0,
    UCTL_SLOT_REQUEST_PENDING = 1,
    UCTL_SLOT_RESPONSE_READY  = 2,
} uctl_slot_state_t;

typedef struct {
    volatile uint32_t lock;   /* clients only */
    volatile uint32_t state;  /* uctl_slot_state_t */
    uctl_request_t    req;
    uctl_response_t   resp;
} uctl_shm_t;

/* Wipes secrets in a way the optimizer cannot drop. */
static inline void uctl_scrub(void *p, size_t n) {
    volatile unsigned char *v = (volatile unsigned char *)p;
    while (n--) *v++ = 0;
}

static inline int uctl_lock(uctl_shm_t *shm, uint32_t timeout_ms) {
    uint64_t deadline = time_ms() + timeout_ms;
    for (;;) {
        uint32_t expected = 0;
        if (__atomic_compare_exchange_n(&shm->lock, &expected, 1, 0,
                                        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
            return 0;
        if (time_ms() >= deadline) return -1;
        yield();
    }
}

static inline void uctl_unlock(uctl_shm_t *shm) {
    __atomic_store_n(&shm->lock, 0, __ATOMIC_RELEASE);
}

static inline uctl_slot_state_t uctl_get_state(uctl_shm_t *shm) {
    return (uctl_slot_state_t)__atomic_load_n(&shm->state, __ATOMIC_ACQUIRE);
}

static inline void uctl_set_state(uctl_shm_t *shm, uctl_slot_state_t s) {
    __atomic_store_n(&shm->state, (uint32_t)s, __ATOMIC_RELEASE);
}

/* Maps the service mailbox. Returns NULL when userctl is not running. */
static inline uctl_shm_t *uctl_attach(void) {
    int h = shm_open(UCTL_IPC_SHM_NAME, O_RDWR, 0, 0);
    if (h < 0) return NULL;
    void *m = mmap(NULL, sizeof(uctl_shm_t), PROT_R | PROT_W, MAP_SHARED, h);
    return (m == MAP_FAILED) ? NULL : (uctl_shm_t *)m;
}

/* One round trip. Returns resp->status, or -1 if the mailbox stayed busy or
 * the service never answered. The request is scrubbed from the slot (and
 * the caller's copy is left to the caller) once the call completes. */
static inline int uctl_call(uctl_shm_t *shm, const uctl_request_t *req,
                            uctl_response_t *out) {
    if (uctl_lock(shm, UCTL_IPC_LOCK_TIMEOUT_MS) != 0) return -1;

    memcpy(&shm->req, req, sizeof(*req));
    uctl_set_state(shm, UCTL_SLOT_REQUEST_PENDING);

    uint64_t deadline = time_ms() + UCTL_IPC_RESPONSE_WAIT_MS;
    int got = 0;
    while (time_ms() < deadline) {
        if (uctl_get_state(shm) == UCTL_SLOT_RESPONSE_READY) { got = 1; break; }
        yield();
    }

    if (got) memcpy(out, &shm->resp, sizeof(*out));
    uctl_scrub(&shm->req, sizeof(shm->req));
    uctl_set_state(shm, UCTL_SLOT_IDLE);
    uctl_unlock(shm);

    return got ? out->status : -1;
}

/* uid -> username. Returns 0 on success. */
static inline int uctl_name_of_uid(uctl_shm_t *shm, uint64_t uid,
                                   char *out, size_t out_sz) {
    uctl_request_t req;
    uctl_response_t resp;
    memset(&req, 0, sizeof(req));
    req.cmd = UCTL_CMD_GETUSERNAME;
    req.intarg = uid;
    if (uctl_call(shm, &req, &resp) != 0) return -1;
    strlcpy(out, resp.name, out_sz);
    return 0;
}
