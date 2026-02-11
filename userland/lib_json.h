#pragma once
#include <stddef.h>

// Minimal JSON support for Pakman manifest.
// Supports parsing/writing: [ {"name":"...","version":"...","installPath":"..."}, ... ]

typedef struct {
    char name[64];
    char version[32];
    char install_path[256];
} pakman_pkg_t;

// Returns number of packages parsed, or -1 on error.
int json_manifest_parse(const char *json, size_t len, pakman_pkg_t *out, int max_out);

// Writes JSON to outbuf. Returns bytes written (excluding NUL), or -1 on error.
int json_manifest_write(const pakman_pkg_t *pkgs, int count, char *outbuf, size_t outcap);
