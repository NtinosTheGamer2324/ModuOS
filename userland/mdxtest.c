#include "libc.h"
#include "string.h"
#include "mdx_proto.h"

static void dbg_u32(const char *label, uint32_t v) {
    puts_raw(label);
    char buf[16];
    int i = 0;
    if (v == 0) {
        buf[i++] = '0';
    } else {
        char tmp[16];
        int t = 0;
        while (v && t < (int)sizeof(tmp)) {
            tmp[t++] = (char)('0' + (v % 10u));
            v /= 10u;
        }
        while (t--) buf[i++] = tmp[t];
    }
    buf[i] = 0;
    puts_raw(buf);
    puts_raw("\n");
}

static void dbg_i32(const char *label, int32_t v) {
    puts_raw(label);
    char buf[20];
    int i = 0;
    if (v < 0) { buf[i++] = '-'; v = -v; }
    uint32_t u = (uint32_t)v;
    if (u == 0) buf[i++] = '0';
    else {
        char tmp[16];
        int t = 0;
        while (u && t < (int)sizeof(tmp)) { tmp[t++] = (char)('0' + (u % 10u)); u /= 10u; }
        while (t--) buf[i++] = tmp[t];
    }
    buf[i] = 0;
    puts_raw(buf);
    puts_raw("\n");
}

static int send_msg_client(int fd, uint16_t type, const void *payload, uint32_t payload_len) {
    dbg_i32("client: send fd=", fd);
    dbg_u32("client: send type=", type);
    dbg_u32("client: payload_len=", payload_len);

    uint8_t buf[512];
    if (payload_len > (sizeof(buf) - sizeof(mdx_msg_hdr_t))) return -1;

    mdx_msg_hdr_t h;
    memset(&h, 0, sizeof(h));
    h.magic = MDX_GUI_MAGIC;
    h.type = type;
    h.size = (uint32_t)(sizeof(mdx_msg_hdr_t) + payload_len);

    memcpy(buf, &h, sizeof(h));
    if (payload_len) memcpy(buf + sizeof(h), payload, payload_len);

    ssize_t n = write(fd, buf, h.size);
    dbg_i32("client: write ret=", (int32_t)n);
    return (n == (ssize_t)h.size) ? 0 : -1;
}

static int send_msg_server(int fd, uint32_t dst_pid, uint16_t type, const void *payload, uint32_t payload_len) {
    uint8_t buf[512];
    if (payload_len > (sizeof(buf) - sizeof(mdx_msg_hdr_t))) return -1;

    mdx_msg_hdr_t h;
    memset(&h, 0, sizeof(h));
    h.magic = MDX_GUI_MAGIC;
    h.type = type;
    h.size = (uint32_t)(sizeof(mdx_msg_hdr_t) + payload_len);
    h.dst_pid = dst_pid;

    memcpy(buf, &h, sizeof(h));
    if (payload_len) memcpy(buf + sizeof(h), payload, payload_len);

    ssize_t n = write(fd, buf, h.size);
    dbg_i32("server: write ret=", (int32_t)n);
    return (n == (ssize_t)h.size) ? 0 : -1;
}

static int server_once(void) {
    dbg_i32("server: pid=", getpid());

    puts_raw("server: open gui0...\n");
    int fd = open("$/dev/gui0", O_RDWR, 0);
    dbg_i32("server: open fd=", fd);
    if (fd < 0) {
        puts_raw("mdxtest(server): cannot open $/dev/gui0\n");
        return 1;
    }

    /* Claim deterministic server role */
    {
        mdx_msg_hdr_t h;
        memset(&h, 0, sizeof(h));
        h.magic = MDX_GUI_MAGIC;
        h.type = MDX_MSG_CLAIM_SERVER;
        h.size = (uint32_t)sizeof(h);
        ssize_t wn = write(fd, &h, sizeof(h));
        dbg_i32("server: claim write ret=", (int32_t)wn);
    }

    int got_hello = 0;
    int got_ping = 0;

    uint64_t start = time_ms();
    for (;;) {
        uint8_t buf[512];
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n < (ssize_t)sizeof(mdx_msg_hdr_t)) {
            if ((time_ms() - start) > 2000) {
                puts_raw("server: timeout waiting for request\n");
                close(fd);
                return 2;
            }
            yield();
            continue;
        }
        dbg_i32("server: read ret=", (int32_t)n);

        mdx_msg_hdr_t *h = (mdx_msg_hdr_t*)buf;
        if (h->magic != MDX_GUI_MAGIC) continue;
        dbg_u32("server: msg type=", h->type);
        dbg_u32("server: msg size=", h->size);
        dbg_u32("server: msg src_pid=", h->src_pid);

        uint32_t payload_len = h->size - (uint32_t)sizeof(mdx_msg_hdr_t);
        uint8_t *payload = buf + sizeof(mdx_msg_hdr_t);

        if (h->type == MDX_MSG_HELLO && payload_len >= sizeof(mdx_hello_t)) {
            mdx_hello_t *hi = (mdx_hello_t*)payload;

            mdx_hello_ack_t ack;
            ack.version = hi->version;
            ack.server_caps = 0;
            (void)send_msg_server(fd, h->src_pid, MDX_MSG_HELLO_ACK, &ack, sizeof(ack));
            got_hello = 1;
        } else if (h->type == MDX_MSG_PING) {
            (void)send_msg_server(fd, h->src_pid, MDX_MSG_PONG, NULL, 0);
            got_ping = 1;
        }

        if (got_hello && got_ping) {
            close(fd);
            return 0;
        }
    }
}

static int client_run(int timeout_ms) {
    dbg_i32("client: pid=", getpid());

    puts_raw("client: open gui0 (nonblock)...\n");
    int fd = open("$/dev/gui0", O_RDWR | O_NONBLOCK, 0);
    dbg_i32("client: open fd=", fd);
    if (fd < 0) {
        puts_raw("mdxtest(client): cannot open $/dev/gui0\n");
        return 1;
    }

    mdx_hello_t hi;
    hi.version = 1;
    (void)send_msg_client(fd, MDX_MSG_HELLO, &hi, sizeof(hi));
    (void)send_msg_client(fd, MDX_MSG_PING, NULL, 0);

    int got_ack = 0;
    int got_pong = 0;

    uint64_t start = time_ms();
    while ((int)(time_ms() - start) < timeout_ms) {
        uint8_t buf[512];
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n == 0) { yield(); continue; }
        if (n < (ssize_t)sizeof(mdx_msg_hdr_t)) continue;
        dbg_i32("client: read ret=", (int32_t)n);

        mdx_msg_hdr_t *h = (mdx_msg_hdr_t*)buf;
        if (h->magic != MDX_GUI_MAGIC) continue;
        dbg_u32("client: msg type=", h->type);
        dbg_u32("client: msg size=", h->size);
        dbg_u32("client: msg src_pid=", h->src_pid);

        if (h->type == MDX_MSG_HELLO_ACK) got_ack = 1;
        else if (h->type == MDX_MSG_PONG) got_pong = 1;

        if (got_ack && got_pong) {
            close(fd);
            return 0;
        }
    }

    close(fd);
    puts_raw("mdxtest: timeout waiting for server replies\n");
    return 2;
}

int md_main(long argc, char **argv) {
    (void)argc;
    (void)argv;

    dbg_i32("mdxtest: pid=", getpid());
    puts_raw("mdxtest v2: fork server + client test for $/dev/gui0\n");

    int pid = fork();
    dbg_i32("mdxtest: fork ret=", pid);
    if (pid < 0) {
        puts_raw("mdxtest: fork failed\n");
        return 1;
    }

    if (pid == 0) {
        /* child: server */
        int rc = server_once();
        return rc;
    }
    printf("mdxtest: started server once\n");

    /* parent: give child time to become server (gui0 picks server by first reader) */
    /* Server role is deterministic now (CLAIM_SERVER), no need to yield. */


    /* parent: client */
    int rc = client_run(1500);
    printf("mdxtest: Client Run\n");

    /* Don't waitpid() here at all.
     * The kernel's process reaping / wait semantics are still being stabilized,
     * and mdxtest is meant to test gui0 IPC, not waitpid.
     */
    (void)pid;

    if (rc == 0) {
        puts_raw("mdxtest: OK (HELLO_ACK + PONG)\n");
        puts_raw("mdxtest: DONE (exiting now)\n");
        return 0;
    }

    puts_raw("mdxtest: FAILED\n");
    puts_raw("mdxtest: DONE (exiting now)\n");
    return rc;
}
