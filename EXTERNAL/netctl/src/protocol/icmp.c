/*
 * icmp.c  –  ICMP (RFC 792) for ModuOS netctl
 *
 * Handles:
 *   - Echo Request (ping) → send Echo Reply
 *
 * Sending:
 *   - icmp_send_echo_request() for outbound pings
 */

#define LIBC_NO_START
#include "libc.h"
#include "netctl.h"
#include "protocol/ip.h"
#include "protocol/icmp.h"

/* ── RX ───────────────────────────────────────────────────────────────── */
void icmp_rx(const ip_hdr_t *iph, const uint8_t *payload, size_t payload_len) {
    if (payload_len < sizeof(icmp_hdr_t)) return;

    const icmp_hdr_t *icmp = (const icmp_hdr_t *)payload;

    switch (icmp->type) {
        case ICMP_ECHO_REQUEST: {
            /* Build an ICMP Echo Reply.
             * The reply is identical to the request except type=0 and
             * the checksum is recomputed.
             */
            static uint8_t reply_buf[ETH_MAX_PAYLOAD];

            if (payload_len > sizeof(reply_buf)) return;

            memcpy(reply_buf, payload, payload_len);
            icmp_hdr_t *rep = (icmp_hdr_t *)reply_buf;
            rep->type     = ICMP_ECHO_REPLY;
            rep->checksum = 0;
            rep->checksum = inet_checksum(reply_buf, payload_len);

            ip_send(iph->src_ip, IP_PROTO_ICMP, reply_buf, payload_len);
            break;
        }

        case ICMP_ECHO_REPLY:
            /* A reply arrived — useful for the ping utility.
             * For now just log it; a real implementation would signal
             * the waiting sender via a callback or shared state.
             */
            printf("[icmp] Echo Reply from ");
            netctl_print_ip(iph->src_ip);
            printf("  id=%u seq=%u\n",
                   ntohs(icmp->id), ntohs(icmp->seq));
            break;

        default:
            /* Unhandled ICMP type — drop */
            break;
    }
}

/* ── TX ───────────────────────────────────────────────────────────────── */
int icmp_send_echo_request(uint32_t dst_ip, uint16_t id, uint16_t seq) {
    /* Minimal echo request: 8-byte header + 32 bytes of data payload */
    static const char icmp_data[32] = "netctl-ping-payload-____________";

    size_t total = sizeof(icmp_hdr_t) + sizeof(icmp_data);
    static uint8_t buf[sizeof(icmp_hdr_t) + sizeof(icmp_data)];

    icmp_hdr_t *icmp = (icmp_hdr_t *)buf;
    icmp->type     = ICMP_ECHO_REQUEST;
    icmp->code     = 0;
    icmp->checksum = 0;
    icmp->id       = htons(id);
    icmp->seq      = htons(seq);

    memcpy(buf + sizeof(icmp_hdr_t), icmp_data, sizeof(icmp_data));

    icmp->checksum = inet_checksum(buf, total);

    return ip_send(dst_ip, IP_PROTO_ICMP, buf, total);
}

/* ── Protocol registration ────────────────────────────────────────────── */
static const ip_proto_handler_t g_icmp_handler = {
    .protocol = IP_PROTO_ICMP,
    .name     = "ICMP",
    .rx       = icmp_rx,
};

void icmp_register(void) {
    ip_proto_register(&g_icmp_handler);
}