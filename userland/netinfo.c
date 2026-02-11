#include "libc.h"
#include "errno.h"

static void print_ipv4_be(uint32_t ip_be) {
    // ip_be is network-order.
    uint8_t a = (uint8_t)((ip_be >> 24) & 0xFF);
    uint8_t b = (uint8_t)((ip_be >> 16) & 0xFF);
    uint8_t c = (uint8_t)((ip_be >> 8) & 0xFF);
    uint8_t d = (uint8_t)((ip_be >> 0) & 0xFF);

    char buf[64];
    // minimal formatting (no snprintf in this libc)
    puts_raw("IP: ");
    itoa(a, buf, 10); puts_raw(buf); puts_raw(".");
    itoa(b, buf, 10); puts_raw(buf); puts_raw(".");
    itoa(c, buf, 10); puts_raw(buf); puts_raw(".");
    itoa(d, buf, 10); puts_raw(buf);
    puts_raw("\n");
}

int md_main(long argc, char **argv) {
    (void)argc; (void)argv;

    long up = net_link_up();
    if (up < 0) {
        puts_raw("netinfo: networking not available (sys_net_link_up failed)\n");
        return 1;
    }

    puts_raw("Link: ");
    puts_raw(up ? "up\n" : "down\n");

    uint32_t ip = 0;
    long r = net_ipv4_addr(&ip);
    if (r == -ENOSYS) {
        puts_raw("IPv4: ENOSYS (no net module implementation)\n");
        return 0;
    }
    if (r < 0) {
        puts_raw("IPv4: error\n");
        return 1;
    }

    print_ipv4_be(ip);

    return 0;
}
