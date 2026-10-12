#include "ps5_audio.h"
#include "receiver_log.h"

#include "airplay_config.h"
#include "ps5_sce.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define PS5_AUDIO_RATE 48000u
#define PS5_AUDIO_CHANNELS 2u
#define PS5_AUDIO_GRAIN 256u
#define PS5_AUDIO_QUEUE_FRAMES 9600u
#define Q32_ONE (UINT64_C(1) << 32)

static pthread_t s_thread;
static int s_thread_running;
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_ready = PTHREAD_COND_INITIALIZER;
static int16_t s_queue[PS5_AUDIO_QUEUE_FRAMES * PS5_AUDIO_CHANNELS];
static size_t s_queue_read;
static size_t s_queue_frames;
static int s_stop;

static uint64_t s_step_q32;
static uint64_t s_phase_q32;
static int16_t s_previous[PS5_AUDIO_CHANNELS];
static int s_have_previous;

static int s_host_fd = -1;
static struct sockaddr_in s_host_address;
static int16_t s_host_block[PS5_AUDIO_GRAIN * PS5_AUDIO_CHANNELS];
static size_t s_host_frames;

static void host_open(void) {
    if (s_host_fd >= 0) {
        return;
    }
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return;
    }
    const int flags = fcntl(fd, F_GETFL, 0);
    (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    memset(&s_host_address, 0, sizeof(s_host_address));
    s_host_address.sin_family = AF_INET;
    s_host_address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    s_host_address.sin_port = htons(AIRPLAY_HOST_AUDIO_PORT);
    s_host_fd = fd;
}

static void host_close(void) {
    if (s_host_fd >= 0) {
        close(s_host_fd);
        s_host_fd = -1;
    }
    s_host_frames = 0;
}

static void host_frame(int16_t left, int16_t right) {
    s_host_block[s_host_frames * 2] = left;
    s_host_block[s_host_frames * 2 + 1] = right;
    if (++s_host_frames < PS5_AUDIO_GRAIN) {
        return;
    }
    s_host_frames = 0;
    if (s_host_fd >= 0) {
        (void)sendto(s_host_fd, s_host_block, sizeof(s_host_block), 0,
                     (const struct sockaddr *)&s_host_address, sizeof(s_host_address));
    }
}

static int16_t clamp16(int32_t value) {
    if (value < -32768) return -32768;
    if (value > 32767) return 32767;
    return (int16_t)value;
}

static void queue_frame(int16_t left, int16_t right) {
    if (s_queue_frames == PS5_AUDIO_QUEUE_FRAMES) {
        s_queue_read = (s_queue_read + 1) % PS5_AUDIO_QUEUE_FRAMES;
        s_queue_frames--;
    }
    const size_t write = (s_queue_read + s_queue_frames) % PS5_AUDIO_QUEUE_FRAMES;
    s_queue[write * 2] = left;
    s_queue[write * 2 + 1] = right;
    s_queue_frames++;
}

static int open_port(void) {
    (void)sceAudioOutInit();
    const int handle = sceAudioOutOpen(PS5_VIDEO_OUT_USER_SYSTEM,
                                       SCE_AUDIO_OUT_PORT_TYPE_MAIN, 0,
                                       PS5_AUDIO_GRAIN, PS5_AUDIO_RATE,
                                       SCE_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
    if (handle < 0) {
        receiver_log("audio", "sceAudioOutOpen failed: 0x%x (sound still goes to the display host)\n",
                     handle);
        return -1;
    }
    int volume[2] = {SCE_AUDIO_OUT_VOLUME_0DB, SCE_AUDIO_OUT_VOLUME_0DB};
    (void)sceAudioOutSetVolume(handle, 0x3, volume);
    receiver_log("audio", "PS5 audio-out ready: %u Hz stereo grain=%u\n",
                 PS5_AUDIO_RATE, PS5_AUDIO_GRAIN);
    return handle;
}

/* The console's audio calls block (and have hung), so only this thread makes
 * them.  The port is optional: with an app in front the console does not play
 * it, and the sound that is heard goes through the display host. */
static void *output_thread(void *arg) {
    static int16_t grain[PS5_AUDIO_GRAIN * PS5_AUDIO_CHANNELS];
    int handle = -1;
    time_t retry_at = 0;
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&s_lock);
        while (!s_stop && s_queue_frames < PS5_AUDIO_GRAIN) {
            pthread_cond_wait(&s_ready, &s_lock);
        }
        if (s_stop) {
            pthread_mutex_unlock(&s_lock);
            if (handle >= 0) {
                (void)sceAudioOutClose(handle);
            }
            return NULL;
        }
        for (size_t i = 0; i < PS5_AUDIO_GRAIN; i++) {
            const size_t read = (s_queue_read + i) % PS5_AUDIO_QUEUE_FRAMES;
            grain[i * 2] = s_queue[read * 2];
            grain[i * 2 + 1] = s_queue[read * 2 + 1];
        }
        s_queue_read = (s_queue_read + PS5_AUDIO_GRAIN) % PS5_AUDIO_QUEUE_FRAMES;
        s_queue_frames -= PS5_AUDIO_GRAIN;
        pthread_mutex_unlock(&s_lock);

        if (handle < 0) {
            const time_t now = time(NULL);
            if (now < retry_at) {
                continue;
            }
            retry_at = now + 10;
            if ((handle = open_port()) < 0) {
                continue;
            }
        }
        const int result = sceAudioOutOutput(handle, grain);
        if (result < 0) {
            receiver_log("audio", "sceAudioOutOutput failed: 0x%x\n", result);
            (void)sceAudioOutClose(handle);
            handle = -1;
            retry_at = time(NULL) + 10;
        }
    }
}

static void reset_resampler(void) {
    s_step_q32 = 0;
    s_phase_q32 = 0;
    s_previous[0] = 0;
    s_previous[1] = 0;
    s_have_previous = 0;
}

int ps5_audio_init(void) {
    ps5_audio_flush();
    host_open();
    if (s_thread_running) {
        return 0;
    }
    s_stop = 0;
    if (pthread_create(&s_thread, NULL, output_thread, NULL) != 0) {
        receiver_log("audio", "audio output thread failed, errno %d\n", errno);
        return 0;
    }
    (void)pthread_detach(s_thread);
    s_thread_running = 1;
    return 0;
}

void ps5_audio_cleanup(void) {
    if (s_thread_running) {
        pthread_mutex_lock(&s_lock);
        s_stop = 1;
        pthread_cond_signal(&s_ready);
        pthread_mutex_unlock(&s_lock);
        s_thread_running = 0;
    }
    host_close();
    ps5_audio_flush();
}

void ps5_audio_flush(void) {
    pthread_mutex_lock(&s_lock);
    s_queue_read = 0;
    s_queue_frames = 0;
    pthread_mutex_unlock(&s_lock);
    reset_resampler();
}

int ps5_audio_submit_s16(const int16_t *samples, size_t frames,
                         unsigned int sample_rate, unsigned int channels) {
    if (!samples || frames == 0 || channels == 0) return -1;
    if (sample_rate == 0) sample_rate = 44100;
    if (channels > 2) channels = 2;
    s_step_q32 = ((uint64_t)sample_rate << 32) / PS5_AUDIO_RATE;
    if (s_step_q32 == 0) s_step_q32 = 1;

    static int peak;
    static size_t counted;
    for (size_t i = 0; i < frames * channels; i++) {
        const int level = samples[i] < 0 ? -samples[i] : samples[i];
        if (level > peak) peak = level;
    }
    counted += frames;
    if (counted >= (size_t)sample_rate * 5) {
        receiver_log("audio", "last %zu samples: peak %d of 32767\n", counted, peak);
        counted = 0;
        peak = 0;
    }

    pthread_mutex_lock(&s_lock);
    for (size_t i = 0; i < frames; i++) {
        const int16_t left = samples[i * channels];
        const int16_t right = channels == 1 ? left : samples[i * channels + 1];
        if (!s_have_previous) {
            s_previous[0] = left;
            s_previous[1] = right;
            s_have_previous = 1;
            continue;
        }
        while (s_phase_q32 < Q32_ONE) {
            const int32_t dl = (int32_t)left - s_previous[0];
            const int32_t dr = (int32_t)right - s_previous[1];
            const int32_t l = s_previous[0] +
                (int32_t)((dl * (int64_t)s_phase_q32) >> 32);
            const int32_t r = s_previous[1] +
                (int32_t)((dr * (int64_t)s_phase_q32) >> 32);
            queue_frame(clamp16(l), clamp16(r));
            host_frame(clamp16(l), clamp16(r));
            s_phase_q32 += s_step_q32;
        }
        s_phase_q32 -= Q32_ONE;
        s_previous[0] = left;
        s_previous[1] = right;
    }
    pthread_cond_signal(&s_ready);
    pthread_mutex_unlock(&s_lock);
    return 0;
}
