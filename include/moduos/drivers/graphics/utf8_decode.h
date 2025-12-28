#pragma once
#include <stdint.h>
#include <stddef.h>

/* Decode one UTF-8 codepoint from s (up to n bytes). Returns bytes consumed or 0 on invalid/incomplete. */
size_t utf8_decode_one(const char *s, size_t n, uint32_t *out_cp);
