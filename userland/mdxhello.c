#include "libc.h"
#include "string.h"
#include "mdx_proto.h"

/*
 * mdxhello.sqr
 *
 * Minimal GUI client that talks to mdxserver via $/dev/gui0.
 */

static int send_msg(int fd, uint16_t type, const void *payload, uint32_t payload_len) {
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
    return (n == (ssize_t)h.size) ? 0 : -1;
}

int md_main(long argc, char **argv) {
    (void)argc;
    (void)argv;

    int fd = open("$/dev/gui0", O_RDWR | O_NONBLOCK, 0);
    if (fd < 0) {
        puts_raw("mdxhello: cannot open $/dev/gui0\n");
        return 1;
    }

    mdx_hello_t hi;
    hi.version = 1;
    (void)send_msg(fd, MDX_MSG_HELLO, &hi, sizeof(hi));
    (void)send_msg(fd, MDX_MSG_PING, NULL, 0);

    puts_raw("mdxhello: sent HELLO+PING, waiting for replies...\n");

    uint8_t buf[512];
    for (;;) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n == 0) { yield(); continue; }
        if (n < (ssize_t)sizeof(mdx_msg_hdr_t)) continue;

        mdx_msg_hdr_t *h = (mdx_msg_hdr_t*)buf;
        if (h->magic != MDX_GUI_MAGIC) continue;

        if (h->type == MDX_MSG_HELLO_ACK) {
            puts_raw("mdxhello: got HELLO_ACK from server pid=");
            char tmp[16];
            itoa((int)h->src_pid, tmp, 10);
            puts_raw(tmp);
            puts_raw("\n");
            return 0;
        } else if (h->type == MDX_MSG_PONG) {
            puts_raw("mdxhello: got PONG\n");
        }
    }
}
