#include "moduos/kernel/audio.h"
#include "moduos/fs/devfs.h"
#include "moduos/kernel/COM/com.h"
#include "moduos/kernel/memory/memory.h" /* kmalloc/kfree */
#include "moduos/kernel/memory/string.h"

/* Very small v1 audio registry: expose a PCM device as $/dev/audio/<name> */

typedef struct {
    const audio_pcm_ops_t *ops;
    void *ctx;
} audio_dev_ctx_t;

static ssize_t audio_dev_read(void *ctx, void *buf, size_t count) {
    (void)ctx; (void)buf; (void)count;
    return 0;
}

static ssize_t audio_dev_write(void *ctx, const void *buf, size_t count) {
    audio_dev_ctx_t *d = (audio_dev_ctx_t*)ctx;
    if (!d || !d->ops || !d->ops->write) return -1;
    return d->ops->write(d->ctx, buf, count);
}

static int audio_dev_close(void *ctx) {
    audio_dev_ctx_t *d = (audio_dev_ctx_t*)ctx;
    if (!d || !d->ops) return 0;
    if (d->ops->close) return d->ops->close(d->ctx);
    return 0;
}

static const devfs_device_ops_t audio_dev_ops = {
    .name = NULL,
    .read = audio_dev_read,
    .write = audio_dev_write,
    .close = audio_dev_close,
    .can_replace = NULL,
};

int audio_register_pcm(const char *dev_name, const audio_pcm_ops_t *ops, void *ctx) {
    if (!dev_name || !*dev_name || !ops || !ops->write) return -1;

    /* create $/dev/audio */
    devfs_owner_t owner = { .kind = DEVFS_OWNER_KERNEL, .id = "kernel" };
    devfs_mkdir_p("audio", owner);

    audio_dev_ctx_t *d = (audio_dev_ctx_t*)kmalloc(sizeof(audio_dev_ctx_t));
    if (!d) return -2;
    d->ops = ops;
    d->ctx = ctx;

    char path[64];
    path[0] = 0;
    strcat(path, "audio/");
    strcat(path, dev_name);

    devfs_device_ops_t ops_copy = audio_dev_ops;
    ops_copy.name = dev_name;

    int r = devfs_register_path(path, &ops_copy, d, owner);
    if (r != 0) {
        kfree(d);
        return -3;
    }

    com_write_string(COM1_PORT, "[AUDIO] Registered PCM device: $/dev/");
    com_write_string(COM1_PORT, path);
    com_write_string(COM1_PORT, "\n");

    return 0;
}
