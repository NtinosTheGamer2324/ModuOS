#ifndef AUDIO_TYPES_H
#define AUDIO_TYPES_H

#include <stdint.h>

/* Sample formats carried on the ring. Kept separate from AC'97-specific
 * audio_format_t in the kernel sqrm.h — this is the audio *manager's*
 * contract with userland, not any one codec's register format. */
typedef enum {
    AUD_FMT_S16_LE = 1,
    AUD_FMT_S32_LE = 2,
    AUD_FMT_F32_LE = 3,
} aud_format_t;

typedef struct {
    uint32_t sample_rate;
    uint16_t channels;
    aud_format_t format;
} aud_pcm_config_t;

typedef enum {
    AUDDEV_TYPE_OUTPUT,
    AUDDEV_TYPE_INPUT,
} auddev_type_t;

typedef struct {
    uint32_t id;
    const char *name;
    auddev_type_t type;
    aud_pcm_config_t preferred;
} audio_controller_t;

typedef enum {
    aesuccess,
    aefail,
    aeperm,
    aeinvalinfo,
    aeoutofspace,
} aerrc_t;

#endif /* AUDIO_TYPES_H */