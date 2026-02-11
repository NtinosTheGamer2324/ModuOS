#include "libc.h"
#include "string.h"
#include "mdx_proto.h"

/*
 * mdxserver.sqr
 *
 * Minimal test server for $/dev/gui0. For now it only handles HELLO and PING.
 * Later this will become the real display server (compositor + input).
 */

static int send_msg(int fd, uint32_t dst_pid, uint16_t type, const void *payload, uint32_t payload_len) {
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
    return (n == (ssize_t)h.size) ? 0 : -1;
}

int md_main(long argc, char **argv) {
    (void)argc;
    (void)argv;

    int fd = open("$/dev/gui0", O_RDWR, 0);
    if (fd < 0) {
        puts_raw("mdxserver: cannot open $/dev/gui0\n");
        return 1;
    }

    /* Claim deterministic server role */
    {
        mdx_msg_hdr_t h;
        memset(&h, 0, sizeof(h));
        h.magic = MDX_GUI_MAGIC;
        h.type = MDX_MSG_CLAIM_SERVER;
        h.size = (uint32_t)sizeof(h);
        (void)write(fd, &h, sizeof(h));
    }

    puts_raw("mdxserver: waiting for requests on $/dev/gui0 ...\n");

    uint8_t buf[512];
    for (;;) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n < (ssize_t)sizeof(mdx_msg_hdr_t)) continue;

        mdx_msg_hdr_t *h = (mdx_msg_hdr_t *)buf;
        if (h->magic != MDX_GUI_MAGIC) continue;

        uint32_t payload_len = h->size - (uint32_t)sizeof(mdx_msg_hdr_t);
        uint8_t *payload = buf + sizeof(mdx_msg_hdr_t);

        if (h->type == MDX_MSG_HELLO && payload_len >= sizeof(mdx_hello_t)) {
            mdx_hello_t *hi = (mdx_hello_t *)payload;
            puts_raw("mdxserver: HELLO from pid=");
            char tmp[16];
            itoa((int)h->src_pid, tmp, 10);
            puts_raw(tmp);
            puts_raw(" version=");
            itoa((int)hi->version, tmp, 10);
            puts_raw(tmp);
            puts_raw("\n");

            mdx_hello_ack_t ack;
            ack.version = hi->version;
            ack.server_caps = 0;
            (void)send_msg(fd, h->src_pid, MDX_MSG_HELLO_ACK, &ack, sizeof(ack));
        } else if (h->type == MDX_MSG_PING) {
            (void)send_msg(fd, h->src_pid, MDX_MSG_PONG, NULL, 0);
        }
    }
}
