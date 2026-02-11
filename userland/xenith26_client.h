#pragma once

#include "libc.h"
#include "string.h"
#include "xenith26_proto.h"

static inline int x26_send(int fd, uint32_t dst_pid, uint16_t type, const void *payload, uint32_t payload_len) {
    uint8_t buf[512];
    if (payload_len > (sizeof(buf) - sizeof(x26_hdr_t))) return -1;

    x26_hdr_t h;
    memset(&h, 0, sizeof(h));
    h.magic = X26_MAGIC;
    h.type = type;
    h.size = (uint32_t)(sizeof(x26_hdr_t) + payload_len);
    h.dst_pid = dst_pid;

    memcpy(buf, &h, sizeof(h));
    if (payload_len) memcpy(buf + sizeof(h), payload, payload_len);

    ssize_t n = write(fd, buf, h.size);
    return (n == (ssize_t)h.size) ? 0 : -1;
}
