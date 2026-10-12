/* Runs inside the app started by display_carrier.c.  It dismisses the app's
 * loading screen and plays the receiver's sound: 48 kHz stereo S16 in loopback
 * datagrams of HOST_GRAIN frames (the console mutes a background process while
 * an app is in front).  It must not open the video output, which is unique and
 * belongs to the receiver.  A 4-byte datagram "AP" + delay is the receiver's
 * heartbeat; without it for HOST_SILENCE_SECONDS the app exits. */

#include "airplay_config.h"
#include "ps5_sce.h"
#include "receiver_log.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define HOST_GRAIN 256u
#define HOST_BLOCK_BYTES (HOST_GRAIN * 2u * sizeof(int16_t))
#define HOST_QUEUE_BLOCKS 96u
#define HOST_BLOCKS_FOR_MS(ms) ((unsigned)(ms) * 48000u / 1000u / HOST_GRAIN)
#define HOST_PAUSE_SECONDS 1
#define HOST_SILENCE_SECONDS 4

int sceNetInit(void);

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ready = PTHREAD_COND_INITIALIZER;
static int16_t queue[HOST_QUEUE_BLOCKS][HOST_GRAIN * 2];
static unsigned queue_read;
static unsigned queue_count;
static unsigned long received;
static unsigned long dropped;
static unsigned long ran_dry;
static unsigned cushion_blocks;
static int cushion_changed;

static void *receive_thread(void *arg) {
    (void)arg;
    int fd = -1;
    time_t last_heard = time(NULL);
    for (;;) {
        if (fd < 0) {
            fd = socket(AF_INET, SOCK_DGRAM, 0);
            struct sockaddr_in address;
            memset(&address, 0, sizeof(address));
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            address.sin_port = htons(AIRPLAY_HOST_AUDIO_PORT);
            int reuse = 1;
            if (fd >= 0) {
                (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
            }
            if (fd < 0 || bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
                if (fd >= 0) {
                    close(fd);
                    fd = -1;
                }
                sleep(1);
                continue;
            }
            struct timeval timeout = {.tv_sec = 1, .tv_usec = 0};
            (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
            receiver_log("host", "listening for audio on loopback UDP/%d",
                         AIRPLAY_HOST_AUDIO_PORT);
            last_heard = time(NULL);
        }
        int16_t block[HOST_GRAIN * 2];
        ssize_t count = recv(fd, block, sizeof(block), 0);
        if (count > 0) {
            last_heard = time(NULL);
        }
        if (count == 4 && ((const uint8_t *)block)[0] == 'A' && ((const uint8_t *)block)[1] == 'P') {
            const unsigned delay_ms = ((const uint8_t *)block)[2] | ((unsigned)((const uint8_t *)block)[3] << 8);
            unsigned blocks = HOST_BLOCKS_FOR_MS(delay_ms);
            if (blocks < 2) blocks = 2;
            if (blocks > HOST_QUEUE_BLOCKS - 8) blocks = HOST_QUEUE_BLOCKS - 8;
            pthread_mutex_lock(&lock);
            if (blocks != cushion_blocks) {
                cushion_blocks = blocks;
                cushion_changed = 1;
                pthread_cond_signal(&ready);
            }
            pthread_mutex_unlock(&lock);
        } else if (time(NULL) - last_heard > HOST_SILENCE_SECONDS) {
            receiver_log("host", "no heartbeat from the receiver for %d s; closing",
                         HOST_SILENCE_SECONDS);
            receiver_log_flush();
            exit(0);
        }
        if (count != (ssize_t)HOST_BLOCK_BYTES) {
            continue;
        }
        pthread_mutex_lock(&lock);
        if (queue_count == HOST_QUEUE_BLOCKS) {
            queue_read = (queue_read + 1) % HOST_QUEUE_BLOCKS;
            queue_count--;
            dropped++;
        }
        memcpy(queue[(queue_read + queue_count) % HOST_QUEUE_BLOCKS], block, sizeof(block));
        queue_count++;
        received++;
        pthread_cond_signal(&ready);
        pthread_mutex_unlock(&lock);
    }
    return NULL;
}

static int open_audio(void) {
    (void)sceAudioOutInit();
    int handle = sceAudioOutOpen(PS5_VIDEO_OUT_USER_SYSTEM, SCE_AUDIO_OUT_PORT_TYPE_MAIN,
                                 0, HOST_GRAIN, 48000,
                                 SCE_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
    if (handle < 0) {
        receiver_log("host", "sceAudioOutOpen failed: 0x%x", handle);
        return -1;
    }
    int volume[2] = {SCE_AUDIO_OUT_VOLUME_0DB, SCE_AUDIO_OUT_VOLUME_0DB};
    (void)sceAudioOutSetVolume(handle, 0x3, volume);
    return handle;
}

int main(void) {
    const int splash = sceSystemServiceHideSplashScreen();
    receiver_log("host", "display host running, pid %d, HideSplashScreen -> 0x%x",
                 (int)getpid(), splash);
    (void)sceNetInit();

    cushion_blocks = HOST_BLOCKS_FOR_MS(AIRPLAY_PLAYOUT_DELAY_MS);
    pthread_t thread;
    if (pthread_create(&thread, NULL, receive_thread, NULL) != 0) {
        receiver_log("host", "audio receive thread failed; no sound");
        for (;;) {
            sleep(1);
        }
    }

    int audio = -1;
    unsigned long played = 0;
    static int16_t block[HOST_GRAIN * 2];
    for (;;) {
        pthread_mutex_lock(&lock);
        if (cushion_changed) {
            cushion_changed = 0;
            while (queue_count > cushion_blocks) {
                queue_read = (queue_read + 1) % HOST_QUEUE_BLOCKS;
                queue_count--;
            }
            while (queue_count < cushion_blocks && !cushion_changed) {
                pthread_cond_wait(&ready, &lock);
            }
        }
        if (queue_count == 0) {
            const time_t emptied = time(NULL);
            while (queue_count < cushion_blocks) {
                pthread_cond_wait(&ready, &lock);
            }
            if (played != 0 && time(NULL) - emptied <= HOST_PAUSE_SECONDS) {
                ran_dry++;
            }
        }
        memcpy(block, queue[queue_read], sizeof(block));
        queue_read = (queue_read + 1) % HOST_QUEUE_BLOCKS;
        queue_count--;
        const unsigned long got = received, lost = dropped, dry = ran_dry;
        pthread_mutex_unlock(&lock);

        if (audio < 0 && (audio = open_audio()) < 0) {
            sleep(1);
            continue;
        }
        const int result = sceAudioOutOutput(audio, block);
        if (played == 0 || result < 0 || played % 2000 == 0) {
            receiver_log("host", "audio block %lu -> 0x%x (received %lu, dropped %lu, ran dry %lu times)",
                         played, result, got, lost, dry);
        }
        if (result < 0) {
            sleep(1);
        }
        played++;
    }
}
