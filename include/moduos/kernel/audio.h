#pragma once

#include <stdint.h>
#include <stddef.h>
#include "moduos/fs/fd.h" /* ssize_t */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Minimal kernel audio API (v1): PCM playback.
 *
 * Model:
 * - Audio drivers register one or more PCM output devices.
 * - A PCM device exposes a byte stream; userland writes interleaved PCM frames.
 * - The driver may either block until consumed or buffer internally.
 */

typedef enum {
    AUDIO_FMT_S16_LE = 1,
    AUDIO_FMT_S32_LE = 2,
    AUDIO_FMT_F32_LE = 3,
} audio_format_t;

typedef struct {
    uint32_t sample_rate;     /* e.g., 48000 */
    uint16_t channels;        /* 1=mono, 2=stereo */
    audio_format_t format;    /* sample format */
} audio_pcm_config_t;

typedef struct {
    char name[32];            /* e.g., "hda", "sb16" */
    uint32_t flags;           /* reserved */
    audio_pcm_config_t preferred;
} audio_device_info_t;

typedef struct audio_pcm_dev audio_pcm_dev_t;

typedef struct {
    int (*open)(void *ctx);
    int (*set_config)(void *ctx, const audio_pcm_config_t *cfg);
    ssize_t (*write)(void *ctx, const void *buf, size_t bytes);
    int (*drain)(void *ctx);
    int (*close)(void *ctx);
    int (*get_info)(void *ctx, audio_device_info_t *out);
} audio_pcm_ops_t;

/* Register a PCM output device. Returns 0 on success. */
int audio_register_pcm(const char *dev_name, const audio_pcm_ops_t *ops, void *ctx);

#ifdef __cplusplus
}
#endif
