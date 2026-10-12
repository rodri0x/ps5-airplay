#include "airplay_audio.h"
#include "receiver_log.h"

#include "crypto/aes.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static int nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    return flags < 0 ? -1 : fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int bind_ephemeral(int *out_fd) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    int reuse = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(0);
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        nonblocking(fd) != 0) {
        close(fd);
        return -1;
    }
    *out_fd = fd;
    return 0;
}

int airplay_audio_init(airplay_audio_t *audio) {
    if (!audio) return -1;
    memset(audio, 0, sizeof(*audio));
    audio->data_fd = -1;
    audio->control_fd = -1;
    if (bind_ephemeral(&audio->data_fd) != 0 ||
        bind_ephemeral(&audio->control_fd) != 0) {
        airplay_audio_close(audio);
        return -1;
    }
    return 0;
}

void airplay_audio_close(airplay_audio_t *audio) {
    if (!audio) return;
    if (audio->data_fd >= 0) close(audio->data_fd);
    if (audio->control_fd >= 0) close(audio->control_fd);
    audio->data_fd = -1;
    audio->control_fd = -1;
}

void airplay_audio_set_crypto(airplay_audio_t *audio, const uint8_t key[16],
                              const uint8_t iv[16]) {
    if (!audio || !key || !iv) return;
    memcpy(audio->key, key, 16);
    memcpy(audio->iv, iv, 16);
    audio->have_crypto = 1;
}

void airplay_audio_set_format(airplay_audio_t *audio, uint8_t codec,
                              uint16_t samples_per_frame) {
    if (!audio) return;
    audio->codec = codec;
    audio->samples_per_frame = samples_per_frame;
    audio->have_timestamp = 0;
    receiver_log("audio", "audio format ct=%u spf=%u\n", codec,
            samples_per_frame);
}

void airplay_audio_set_callback(airplay_audio_t *audio,
                                airplay_audio_callback_t callback, void *opaque) {
    if (!audio) return;
    audio->callback = callback;
    audio->callback_opaque = opaque;
}

static void receive_data(airplay_audio_t *audio) {
    for (;;) {
        ssize_t length = recvfrom(audio->data_fd, audio->packet,
                                  sizeof(audio->packet), 0, NULL, NULL);
        if (length < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            return;
        }
        if (length <= 12) continue;
        size_t payload_length = (size_t)length - 12;
        const uint8_t *payload = audio->packet + 12;
        if (payload_length == 4 && payload[0] == 0x00 && payload[1] == 0x68 &&
            payload[2] == 0x34 && payload[3] == 0x00) {
            continue;
        }
        /* "redundantAudio": every packet arrives several times. */
        const uint32_t packet_timestamp = ((uint32_t)audio->packet[4] << 24) |
                                          ((uint32_t)audio->packet[5] << 16) |
                                          ((uint32_t)audio->packet[6] << 8) |
                                          audio->packet[7];
        if (audio->have_timestamp) {
            const int32_t ahead = (int32_t)(packet_timestamp - audio->last_timestamp);
            if (ahead <= 0 && ahead > -(int32_t)(48000 * 5)) {
                audio->duplicates++;
                continue;
            }
        }
        audio->last_timestamp = packet_timestamp;
        audio->have_timestamp = 1;
        if (audio->have_crypto) {
            size_t encrypted = (payload_length / 16u) * 16u;
            memcpy(audio->decoded, payload, payload_length);
            if (encrypted) {
                struct AES_ctx aes;
                AES_init_ctx_iv(&aes, audio->key, audio->iv);
                AES_CBC_decrypt_buffer(&aes, audio->decoded, encrypted);
            }
            payload = audio->decoded;
        }
        audio->packets++;
        audio->bytes += payload_length;
        struct timespec now;
        if (clock_gettime(CLOCK_MONOTONIC, &now) == 0) {
            const uint64_t now_ms = (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
            if (audio->last_arrival_ms != 0 && now_ms - audio->last_arrival_ms < 2000u) {
                static const unsigned int limits[6] = {40, 80, 120, 160, 200, 300};
                const uint64_t gap = now_ms - audio->last_arrival_ms;
                if (gap > audio->longest_gap_ms) {
                    audio->longest_gap_ms = gap;
                }
                for (int i = 0; i < 6; i++) {
                    audio->gap_counts[i] += gap > limits[i];
                }
            }
            audio->last_arrival_ms = now_ms;
        }
        if (audio->packets % 500 == 0) {
            receiver_log("audio", "packets played %llu, repeats dropped %llu, longest gap %llu ms; "
                         "gaps over 40/80/120/160/200/300 ms: %u/%u/%u/%u/%u/%u\n",
                         (unsigned long long)audio->packets,
                         (unsigned long long)audio->duplicates,
                         (unsigned long long)audio->longest_gap_ms,
                         audio->gap_counts[0], audio->gap_counts[1], audio->gap_counts[2],
                         audio->gap_counts[3], audio->gap_counts[4], audio->gap_counts[5]);
            audio->longest_gap_ms = 0;
            memset(audio->gap_counts, 0, sizeof(audio->gap_counts));
        }
        if (audio->callback) {
            uint32_t timestamp = ((uint32_t)audio->packet[4] << 24) |
                                 ((uint32_t)audio->packet[5] << 16) |
                                 ((uint32_t)audio->packet[6] << 8) |
                                 audio->packet[7];
            audio->callback(audio->callback_opaque, payload, payload_length,
                            audio->codec, timestamp);
        }
    }
}

static void drain_control(airplay_audio_t *audio) {
    uint8_t packet[2048];
    for (;;) {
        ssize_t length = recvfrom(audio->control_fd, packet, sizeof(packet), 0,
                                  NULL, NULL);
        if (length < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        if (length <= 0) return;
    }
}

int airplay_audio_poll(airplay_audio_t *audio) {
    if (!audio || audio->data_fd < 0) return -1;
    receive_data(audio);
    drain_control(audio);
    return 0;
}

static int port_of(int fd) {
    struct sockaddr_in address;
    socklen_t length = sizeof(address);
    if (fd < 0 || getsockname(fd, (struct sockaddr *)&address, &length) != 0)
        return -1;
    return ntohs(address.sin_port);
}

int airplay_audio_data_port(const airplay_audio_t *audio) {
    return audio ? port_of(audio->data_fd) : -1;
}

int airplay_audio_control_port(const airplay_audio_t *audio) {
    return audio ? port_of(audio->control_fd) : -1;
}
