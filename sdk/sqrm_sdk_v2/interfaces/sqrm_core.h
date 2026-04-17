#pragma once
/*
 * sqrm_core.h — SQRM ABI core: version constants, module type enum,
 *               descriptor structs, and the SQRM_DEFINE_MODULE* macros.
 *
 * Include this header in every third-party module.
 */

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/*  ABI version constants                                              */
/* ------------------------------------------------------------------ */

#define SQRM_ABI_V1 1u
#define SQRM_ABI_V2 2u

/*
 * Define SQRM_ABI_VERSION before including this header to opt into v2.
 * Defaults to v1 for maximum compatibility.
 */
#ifndef SQRM_ABI_VERSION
#define SQRM_ABI_VERSION SQRM_ABI_V1
#endif

/* Symbol name the kernel module loader looks for. */
#define SQRM_DESC_SYMBOL "sqrm_module_desc"

/* ------------------------------------------------------------------ */
/*  Module type                                                        */
/* ------------------------------------------------------------------ */

typedef enum {
    SQRM_TYPE_INVALID = 0,
    SQRM_TYPE_FS      = 1,
    SQRM_TYPE_DRIVE   = 2,
    SQRM_TYPE_USB     = 3,
    SQRM_TYPE_AUDIO   = 4,
    SQRM_TYPE_GPU     = 5,
    SQRM_TYPE_NET     = 6,
    SQRM_TYPE_HID     = 7,
    SQRM_TYPE_GENERIC = 8,
} sqrm_module_type_t;

/* ------------------------------------------------------------------ */
/*  Module descriptors                                                 */
/* ------------------------------------------------------------------ */

/* ABI v1 descriptor */
typedef struct {
    uint32_t abi_version;         /* must be SQRM_ABI_V1 */
    sqrm_module_type_t type;
    const char *name;             /* short name, e.g. "ext2" */
} sqrm_module_desc_t;

/*
 * ABI v2 descriptor — backward-compatible extension of v1.
 * The first three fields are identical to sqrm_module_desc_t so the
 * kernel can safely read abi_version before casting.
 */
typedef struct {
    /* v1 prefix — must stay first */
    uint32_t abi_version;         /* must be SQRM_ABI_V2 */
    sqrm_module_type_t type;
    const char *name;

    /* v2 additions */
    uint16_t class_id;
    uint16_t subclass_id;
    uint16_t dep_count;
    uint16_t flags;
    const char * const *deps;     /* array of dep_count dependency names */
} sqrm_module_desc_v2_t;

/* ------------------------------------------------------------------ */
/*  Convenience macros                                                 */
/* ------------------------------------------------------------------ */

/*
 * SQRM_DEFINE_MODULE — defines the required `sqrm_module_desc` export (ABI v1).
 *
 * Usage (in exactly one .c file per module):
 *   SQRM_DEFINE_MODULE(SQRM_TYPE_FS, "myfs");
 */
#define SQRM_DEFINE_MODULE(_type, _name_literal) \
    const sqrm_module_desc_t sqrm_module_desc = { \
        .abi_version = SQRM_ABI_V1, \
        .type        = (_type), \
        .name        = (_name_literal), \
    }

/*
 * SQRM_DEFINE_MODULE_V2 — defines the required `sqrm_module_desc` export (ABI v2).
 *
 * Usage:
 *   static const char *my_deps[] = { "usbcore", NULL };
 *   SQRM_DEFINE_MODULE_V2(SQRM_TYPE_USB, "myusb", 0x0C, 0x03, 1, my_deps);
 */
#define SQRM_DEFINE_MODULE_V2(_type, _name_literal, _class_id, _subclass_id, _dep_count, _deps_ptr) \
    const sqrm_module_desc_v2_t sqrm_module_desc = { \
        .abi_version = SQRM_ABI_V2, \
        .type        = (_type), \
        .name        = (_name_literal), \
        .class_id    = (_class_id), \
        .subclass_id = (_subclass_id), \
        .dep_count   = (_dep_count), \
        .flags       = 0, \
        .deps        = (_deps_ptr), \
    }

#ifdef __cplusplus
}
#endif