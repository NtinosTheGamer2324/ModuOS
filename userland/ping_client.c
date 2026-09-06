// ping_client.c — talks only to netd's SHM mailbox, never touches net0
// directly.
//
// Usage: ping_client <target_ip>
//


#include "libc.h"
#include "netd_ipc.h"

static int parse_ipv4(const char *s, uint8_t out[4]) {
    int idx = 0, val = 0, digits = 0;
    for (const char *p = s; ; p++) {
        if (*p >= '0' && *p <= '9') { val = val * 10 + (*p - '0'); digits++; }
        else if (*p == '.' || *p == 0) {
            if (digits == 0 || idx > 3) return -1;
            out[idx++] = (uint8_t)val; val = 0; digits = 0;
            if (*p == 0) break;
        } else return -1;
    }
    return (idx == 4) ? 0 : -1;
}

static void print_ipv4(const uint8_t ip[4]) { printf("%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]); }

int md_main(long argc, char **argv) {
    if (argc < 2) {
        printf("usage: ping_client <target_ip>\n");
        return 1;
    }

    uint8_t target_ip[4];
    if (parse_ipv4(argv[1], target_ip) != 0) {
        printf("ping_client: bad IP '%s'\n", argv[1]);
        return 1;
    }

    // 0 size => attach to netd's existing segment rather than create one.
    int shm_handle = shm_open(NETD_IPC_SHM_NAME, O_RDWR, 0, 0);
    if (shm_handle < 0) {
        printf("ping_client: could not open netd's mailbox (is netd running?)\n");
        return 1;
    }

    netd_shm_t *shm = (netd_shm_t *)mmap(NULL, sizeof(netd_shm_t), PROT_R | PROT_W,
                                          MAP_SHARED, shm_handle);
    if (shm == MAP_FAILED) {
        printf("ping_client: mmap of netd mailbox failed\n");
        return 1;
    }

    netd_request_t req;
    netd_response_t resp;

    memset(&req, 0, sizeof(req));
    req.cmd = NETD_CMD_GET_STATUS;
    memset(&resp, 0, sizeof(resp));
    if (netd_ipc_call(shm, &req, &resp, 3000) == 0 && resp.success) {
        printf("ping_client: netd is up, our_ip=");
        print_ipv4(resp.our_ip);
        printf(" gateway=");
        print_ipv4(resp.gateway_ip);
        printf("\n");
    }

    printf("ping_client: pinging ");
    print_ipv4(target_ip);
    printf(" via netd...\n");

    memset(&req, 0, sizeof(req));
    req.cmd = NETD_CMD_PING;
    memcpy(req.target_ip, target_ip, 4);
    req.timeout_ms = 3000;

    memset(&resp, 0, sizeof(resp));
    // Wait at least as long as netd itself might take (req.timeout_ms)
    // plus a margin, since netd only posts the response after do_ping()
    // finishes or times out.
    int r = netd_ipc_call(shm, &req, &resp, req.timeout_ms + NETD_IPC_RESPONSE_MARGIN_MS);
    if (r == 0 && resp.success) {
        printf("ping_client: reply from ");
        print_ipv4(target_ip);
        printf(": time=%ums\n", resp.rtt_ms);
    } else if (r == 0) {
        printf("ping_client: failed (%s)\n", resp.message[0] ? resp.message : "no response");
    } else {
        printf("ping_client: netd mailbox timed out (netd busy or not responding)\n");
    }

    munmap(shm, sizeof(netd_shm_t));

    return (r == 0 && resp.success) ? 0 : 1;
}