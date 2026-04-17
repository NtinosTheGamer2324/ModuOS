#pragma once
/*
 * sqrm_audio.h — SQRM audio PCM device ABI.
 *
 * Available to: AUDIO modules only.
 * All audio-related fields in sqrm_kernel_api_t (dma_alloc/free, port I/O,
 * irq_install_handler, audio_register_pcm) are NULL for all other module types.
 *
 * An AUDIO module must call api->audio_register_pcm() from sqrm_module_init().
 */

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/*  PCM sample format                                                  */
/* ------------------------------------------------------------------ */

typedef enum {
    AUDIO_FMT_S16_LE = 1,   /* signed 16-bit, little-endian */
    AUDIO_FMT_S32_LE = 2,   /* signed 32-bit, little-endian */
    AUDIO_FMT_F32_LE = 3,   /* IEEE 754 float, little-endian */
} audio_format_t;

/* ------------------------------------------------------------------ */
/*  Stream configuration                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t      sample_rate;
    uint16_t      channels;
    audio_format_t format;
} audio_pcm_config_t;

/* ------------------------------------------------------------------ */
/*  Device info (returned by ops->get_info)                           */
/* ------------------------------------------------------------------ */

typedef struct {
    char              name[32];
    uint32_t          flags;        /* reserved, set to 0 */
    audio_pcm_config_t preferred;   /* driver's preferred config */
} audio_device_info_t;

/* ------------------------------------------------------------------ */
/*  Driver operations table                                            */
/* ------------------------------------------------------------------ */

typedef struct {
    /* open() — initialise hardware; return 0 on success */
    int  (*open)      (void *ctx);

    /* set_config() — apply sample rate / channel / format; return 0 on success */
    int  (*set_config)(void *ctx, const audio_pcm_config_t *cfg);

    /*
     * write() — submit PCM frames.
     * Returns number of bytes consumed, or negative errno on error.
     */
    long (*write)(void *ctx, const void *buf, size_t bytes);

    /* drain() — block until the hardware output buffer is empty */
    int  (*drain)(void *ctx);

    /* close() — shut down the stream */
    int  (*close)(void *ctx);

    /* get_info() — fill *out; return 0 on success */
    int  (*get_info)(void *ctx, audio_device_info_t *out);
} audio_pcm_ops_t;

#ifdef __cplusplus
}
#endif