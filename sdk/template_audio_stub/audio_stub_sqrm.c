#include "../sqrm_sdk.h"

/*
 * Audio SQRM module stub.
 * Registers a dummy PCM output device at $/dev/audio/null.
 * Writing discards audio data.
 */

SQRM_DEFINE_MODULE(SQRM_TYPE_AUDIO, "audio_stub");

static const uint16_t COM1_PORT = 0x3F8;

static long null_write(void *ctx, const void *buf, size_t bytes) {
    (void)ctx; (void)buf;
    return (long)bytes;
}

static int null_get_info(void *ctx, audio_device_info_t *out) {
    (void)ctx;
    if (!out) return -1;
    for (int i = 0; i < (int)sizeof(out->name); i++) out->name[i] = 0;
    out->preferred.sample_rate = 48000;
    out->preferred.channels = 2;
    out->preferred.format = AUDIO_FMT_S16_LE;
    return 0;
}

static const audio_pcm_ops_t ops = {
    .open = NULL,
    .set_config = NULL,
    .write = null_write,
    .drain = NULL,
    .close = NULL,
    .get_info = null_get_info,
};

int sqrm_module_init(const sqrm_kernel_api_t *api) {
    if (!api || api->abi_version != SQRM_ABI_VERSION) return -1;

    if (api->com_write_string) api->com_write_string(COM1_PORT, "[audio_stub] init\n");

    if (!api->audio_register_pcm) {
        if (api->com_write_string) api->com_write_string(COM1_PORT, "[audio_stub] audio_register_pcm not available\n");
        return -2;
    }

    return api->audio_register_pcm("null", &ops, NULL);
}
