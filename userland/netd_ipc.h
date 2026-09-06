// netd_ipc.h — shared-memory IPC between netd and its clients.
//
// UserFS's $/user/* invoke() mechanism only round-trips a single fixed
// in_buf/out_buf pair through the kernel and doesn't scale past that, so
// netd's client interface now lives in a SHM segment instead: one mailbox
// slot (a request, a response, and a tiny state machine) guarded by a
// spinlock so only one client uses it at a time.
//
// v3 adds TCP (see netd.c): TCP_OPEN/SEND/RECV/CLOSE. Unlike
// PING/RESOLVE_ARP/UDP_SEND, a TCP connection outlives any single mailbox
// call -- it's identified by tcp_handle across calls, and netd keeps
// servicing it (retransmits, incoming segments, TIME_WAIT expiry) whether
// or not a client is currently blocked on the mailbox. The mailbox itself
// is still just one slot: only one client call is in flight at a time,
// same locking model as v1/v2.
//
// v4 adds a DHCP client inside netd itself (no new mailbox command --
// it runs before the mailbox is even created, see md_main() in netd.c).
// This isn't optional polish: under passt, unlike SLIRP, there's no
// fixed 10.0.2.15/10.0.2.2 numbering to hardcode -- the guest is expected
// to actually run DHCP to learn its address, gateway, and mask. netd now
// does that at startup (falling back to the old static address if no
// server answers) and re-runs it when the lease is about to expire.
// GET_STATUS's response gained a `netmask` field as part of this.
//
// Server side (netd):
//   shm_open(NETD_IPC_SHM_NAME, O_RDWR | SHM_O_CREAT, 0644, sizeof(netd_shm_t))
//   mmap(..., MAP_SHARED, handle) once, memset to zero, then each loop
//   iteration check netd_ipc_get_state() and, if NETD_SLOT_REQUEST_PENDING,
//   answer via netd_ipc_set_state(NETD_SLOT_RESPONSE_READY).
//
// Client side:
//   shm_open(NETD_IPC_SHM_NAME, O_RDWR, 0, 0)   // 0 size = attach existing
//   mmap(..., MAP_SHARED, handle) once, then netd_ipc_call() per request.

#pragma once
#include "libc.h"

#define NETD_IPC_SHM_NAME "netd_ipc"

// How long a client will spin trying to grab the mailbox lock before
// giving up (e.g. another client is mid-request).
#define NETD_IPC_LOCK_TIMEOUT_MS     2000

// Extra headroom added on top of a request's own timeout_ms while the
// client waits for netd to post a response. netd can legitimately take
// up to the request's own timeout_ms doing ARP/ping/TCP work before it
// answers, so the client's wait has to be at least that long.
#define NETD_IPC_RESPONSE_MARGIN_MS  1000

// ── Wire protocol ────────────────────────────────────────────────────
//
// v1: PING / GET_STATUS / RESOLVE_ARP.
// v2: UDP_SEND -- send one datagram, wait up to timeout_ms for one reply
// datagram back from the same (ip, port). No sockets, no "listen".
// v3: TCP_OPEN/SEND/RECV/CLOSE -- a real (stop-and-wait, no options,
// no simultaneous-close) TCP connection, identified by a handle that
// stays valid across calls until CLOSE.

#define NETD_CMD_PING         1
#define NETD_CMD_GET_STATUS   2
#define NETD_CMD_RESOLVE_ARP  3
#define NETD_CMD_UDP_SEND     4
#define NETD_CMD_TCP_OPEN     5
#define NETD_CMD_TCP_SEND     6
#define NETD_CMD_TCP_RECV     7
#define NETD_CMD_TCP_CLOSE    8

// Payload cap for UDP_SEND requests/replies. Comfortably covers a DNS
// query/response (the reference test case), which is why it was picked;
// bump it if you need bigger datagrams later. The mailbox is still a
// single SHM page either way (shm_open() rounds size up to one), so this
// has plenty of headroom before it'd matter.
#define NETD_UDP_MAX_PAYLOAD  512

// Payload cap per TCP SEND/RECV call. Bigger than NETD_UDP_MAX_PAYLOAD
// since HTTP headers alone can exceed 512B, but it's still just "how
// much moves through one mailbox round trip" -- callers loop over
// multiple SEND/RECV calls for anything larger (see http_client.c).
#define NETD_TCP_MAX_PAYLOAD  1024

typedef struct {
    int      cmd;
    uint8_t  target_ip[4];
    uint32_t timeout_ms;   // used by PING, RESOLVE_ARP, UDP_SEND, TCP_*

    // UDP_SEND (v2):
    uint16_t udp_dst_port;
    uint16_t udp_src_port;      // 0 = let netd pick an ephemeral port
    uint16_t udp_payload_len;   // <= NETD_UDP_MAX_PAYLOAD
    uint8_t  udp_payload[NETD_UDP_MAX_PAYLOAD];

    // TCP (v3):
    uint16_t tcp_dst_port;              // OPEN only
    int      tcp_handle;                // SEND/RECV/CLOSE; ignored by OPEN
    uint16_t tcp_payload_len;           // SEND: bytes to send
    uint8_t  tcp_payload[NETD_TCP_MAX_PAYLOAD]; // SEND: data out
    uint16_t tcp_recv_cap;              // RECV: max bytes wanted, <= NETD_TCP_MAX_PAYLOAD
} netd_request_t;

typedef struct {
    int      success;
    uint32_t rtt_ms;        // PING / UDP_SEND (time to first reply)
    uint8_t  mac[6];        // RESOLVE_ARP / GET_STATUS (our_mac)
    uint8_t  our_ip[4];     // GET_STATUS
    uint8_t  gateway_ip[4]; // GET_STATUS
    uint8_t  netmask[4];    // GET_STATUS (v4: from DHCP, or static fallback)
    uint8_t  link_up;       // GET_STATUS
    uint8_t  dhcp_bound;    // GET_STATUS (v4): 1 = address came from DHCP,
                             // 0 = static fallback (no DHCP server answered)
    char     message[64];

    // UDP_SEND (v2):
    uint16_t udp_reply_len; // <= NETD_UDP_MAX_PAYLOAD
    uint8_t  udp_reply[NETD_UDP_MAX_PAYLOAD];

    // TCP (v3):
    int      tcp_handle;                // OPEN: the new connection's handle
    uint16_t tcp_payload_len;           // RECV: bytes actually copied into tcp_payload
    uint8_t  tcp_payload[NETD_TCP_MAX_PAYLOAD]; // RECV: data in
    uint8_t  tcp_eof;                   // RECV: peer sent FIN and buffer is drained -- no more data ever
} netd_response_t;

// ── Mailbox slot ────────────────────────────────────────────────────────

typedef enum {
    NETD_SLOT_IDLE            = 0, // free, nobody holds it
    NETD_SLOT_REQUEST_PENDING = 1, // client posted req, waiting on netd
    NETD_SLOT_RESPONSE_READY  = 2, // netd posted resp, waiting on client
} netd_slot_state_t;

typedef struct {
    volatile uint32_t lock;   // 0 = free, 1 = held. Only clients CAS this.
    volatile uint32_t state;  // netd_slot_state_t
    netd_request_t    req;
    netd_response_t   resp;
} netd_shm_t;

// ── Spinlock + state helpers (built on GCC atomic builtins — no futex/
//    semaphore syscall exists in this libc, so we cooperate via yield()) ──

static inline int netd_ipc_lock_try(netd_shm_t *shm) {
    uint32_t expected = 0;
    return __atomic_compare_exchange_n(&shm->lock, &expected, 1, 0,
                                        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}

static inline int netd_ipc_lock(netd_shm_t *shm, uint32_t timeout_ms) {
    uint64_t deadline = time_ms() + timeout_ms;
    while (!netd_ipc_lock_try(shm)) {
        if (time_ms() >= deadline) return -1;
        yield();
    }
    return 0;
}

static inline void netd_ipc_unlock(netd_shm_t *shm) {
    __atomic_store_n(&shm->lock, 0, __ATOMIC_RELEASE);
}

static inline netd_slot_state_t netd_ipc_get_state(netd_shm_t *shm) {
    return (netd_slot_state_t)__atomic_load_n(&shm->state, __ATOMIC_ACQUIRE);
}

static inline void netd_ipc_set_state(netd_shm_t *shm, netd_slot_state_t s) {
    __atomic_store_n(&shm->state, (uint32_t)s, __ATOMIC_RELEASE);
}

// ── Client-side convenience: one full request/response round trip ─────
//
// Returns 0 on success (*out_resp filled in), -1 on timeout (either the
// mailbox stayed locked by someone else, or netd never answered within
// wait_ms). wait_ms should be >= req->timeout_ms + NETD_IPC_RESPONSE_MARGIN_MS
// for PING/RESOLVE_ARP/UDP_SEND/TCP_* requests, since netd's own
// processing can take that long.
static inline int netd_ipc_call(netd_shm_t *shm, const netd_request_t *req,
                                 netd_response_t *out_resp, uint32_t wait_ms) {
    if (netd_ipc_lock(shm, NETD_IPC_LOCK_TIMEOUT_MS) != 0) {
        return -1;
    }

    memcpy(&shm->req, req, sizeof(*req));
    netd_ipc_set_state(shm, NETD_SLOT_REQUEST_PENDING);

    uint64_t deadline = time_ms() + wait_ms;
    int got_response = 0;
    while (time_ms() < deadline) {
        if (netd_ipc_get_state(shm) == NETD_SLOT_RESPONSE_READY) {
            got_response = 1;
            break;
        }
        yield();
    }

    if (got_response) memcpy(out_resp, &shm->resp, sizeof(*out_resp));

    netd_ipc_set_state(shm, NETD_SLOT_IDLE);
    netd_ipc_unlock(shm);

    return got_response ? 0 : -1;
}