#include "libc.h"

/*
 * audiotest: generate a 440Hz sine-ish tone (square wave) and write to $/dev/audio/pcm0.
 * Format: 48kHz stereo S16LE.
 */

static int16_t clamp16(int v) {
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

int md_main(long argc, char **argv) {
    (void)argc; (void)argv;

    int fd = open("$/dev/audio/pcm0", O_WRONLY, 0);
    if (fd < 0) {
        printf("audiotest: cannot open $/dev/audio/pcm0\n");
        return 1;
    }

    const int sample_rate = 48000;
    const int hz = 440;
    const int seconds = 2;

    /* 1024 frames per chunk */
    enum { FRAMES = 1024 };
    int16_t buf[FRAMES * 2];

    int total_frames = sample_rate * seconds;
    for (int i = 0; i < total_frames; i += FRAMES) {
        int frames = FRAMES;
        if (i + frames > total_frames) frames = total_frames - i;

        for (int f = 0; f < frames; f++) {
            int t = i + f;
            /* crude square wave */
            int phase = (t * hz) % sample_rate;
            int amp = (phase < (sample_rate / (hz * 2))) ? 12000 : -12000;
            int16_t s = clamp16(amp);
            buf[f * 2 + 0] = s;
            buf[f * 2 + 1] = s;
        }

        write(fd, buf, (size_t)frames * 2 * sizeof(int16_t));
    }

    close(fd);
    printf("audiotest: done\n");
    return 0;
}
