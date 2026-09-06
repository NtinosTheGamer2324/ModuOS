#ifndef AUDIO_CONTRACT_H
#define AUDIO_CONTRACT_H

#include "audiosqrm/types.h"

#define AUDIO_CLASS 4
#define AUDIO_CONTROLLER 1

/*
 * In-kernel service API, published via
 * sqrm_service_register("audiomanager", &audio_api, sizeof(audio_api)).
 *
 * This is the *kernel-side* contract — a hardware driver module (e.g. an
 * AC'97 driver) calls these to register itself and hand PCM frames up.
 * It is separate from the $/dev/audio/aud0 devfs contract, which is what
 * userland talks over (see audmgr_wire.h).
 */
typedef struct {
    /* Generic Standard API */
    int (*register_controller)(auddev_type_t type, const char *name,
                               const aud_pcm_config_t *preferred);
    int (*remove_controller)(int id, const char *name);

    /* Called by an OUTPUT controller (e.g. ac97audio) when it needs more
     * frames to play — pulls from whatever userland last submitted via
     * the ring. Returns frames actually written into buf (may be less
     * than requested if the ring is empty; caller should silence-fill
     * the remainder rather than block, since this can run in IRQ context). */
    uint32_t (*pull_output_frames)(void *buf, uint32_t frame_count);

    /* Called by an INPUT controller (e.g. a mic driver) to hand captured
     * frames to whichever userland session is listening. */
    void (*push_input_frames)(int controllerid, const void *buf, uint32_t frame_count);
} audio_api_t;

#endif /* AUDIO_CONTRACT_H */