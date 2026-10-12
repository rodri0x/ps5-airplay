#include "airplay_mirror.h"
#include "receiver_log.h"

#include "crypto/aes.h"
#include "tweetnacl.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define MIRROR_HEADER_SIZE 128u
#define MIRROR_MAX_PAYLOAD (4u * 1024u * 1024u)

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

/* Packet header, 128 bytes: payload size (LE) at 0, type at 4 (0 video,
 * 1 codec config), sender timestamp at 8 (NTP style, LE: fraction, seconds). */
static uint64_t header_timestamp_us(const uint8_t *header) {
    const uint64_t fraction = le32(header + 8);
    const uint64_t seconds = le32(header + 12);
    return seconds * 1000000u + ((fraction * 1000000u) >> 32);
}

static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static int make_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    return flags < 0 ? -1 : fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void derive_video_cipher(airplay_mirror_t *mirror) {
    if (!mirror->have_audio_key || !mirror->have_stream_id) {
        mirror->have_video_cipher = 0;
        return;
    }
    char label[64];
    uint8_t input[96];
    uint8_t digest[64];
    int length = snprintf(label, sizeof(label), "AirPlayStreamKey%llu",
                          (unsigned long long)mirror->stream_connection_id);
    if (length <= 0 || (size_t)length + sizeof(mirror->audio_key) > sizeof(input)) {
        mirror->have_video_cipher = 0;
        return;
    }
    memcpy(input, label, (size_t)length);
    memcpy(input + length, mirror->audio_key, sizeof(mirror->audio_key));
    crypto_hash(digest, input, (unsigned long long)length + sizeof(mirror->audio_key));
    memcpy(mirror->video_key, digest, sizeof(mirror->video_key));

    length = snprintf(label, sizeof(label), "AirPlayStreamIV%llu",
                      (unsigned long long)mirror->stream_connection_id);
    if (length <= 0 || (size_t)length + sizeof(mirror->audio_key) > sizeof(input)) {
        mirror->have_video_cipher = 0;
        return;
    }
    memcpy(input, label, (size_t)length);
    memcpy(input + length, mirror->audio_key, sizeof(mirror->audio_key));
    crypto_hash(digest, input, (unsigned long long)length + sizeof(mirror->audio_key));
    memcpy(mirror->video_iv, digest, sizeof(mirror->video_iv));
    if (!mirror->aes) {
        mirror->aes = calloc(1, sizeof(*mirror->aes));
    }
    if (!mirror->aes) {
        mirror->have_video_cipher = 0;
        return;
    }
    AES_init_ctx_iv(mirror->aes, mirror->video_key, mirror->video_iv);
    mirror->leftover_keystream = 0;
    mirror->have_video_cipher = 1;
}

static void decrypt_payload(airplay_mirror_t *mirror, uint8_t *payload,
                            size_t length) {
    if (!mirror->have_video_cipher || length == 0) {
        return;
    }
    size_t offset = 0;
    if (mirror->leftover_keystream != 0) {
        const size_t available = 16u - mirror->leftover_keystream;
        const size_t count = length < available ? length : available;
        for (size_t i = 0; i < count; i++) {
            payload[i] ^= mirror->keystream[mirror->leftover_keystream + i];
        }
        mirror->leftover_keystream += count;
        offset += count;
        if (mirror->leftover_keystream == 16u) {
            mirror->leftover_keystream = 0;
        }
    }

    const size_t full = ((length - offset) / 16u) * 16u;
    if (full != 0) {
        AES_CTR_xcrypt_buffer(mirror->aes, payload + offset, full);
        offset += full;
    }
    if (offset < length) {
        memset(mirror->keystream, 0, sizeof(mirror->keystream));
        AES_CTR_xcrypt_buffer(mirror->aes, mirror->keystream,
                              sizeof(mirror->keystream));
        const size_t rest = length - offset;
        for (size_t i = 0; i < rest; i++) {
            payload[offset + i] ^= mirror->keystream[i];
        }
        mirror->leftover_keystream = rest;
    }
}

static int convert_nalus(airplay_mirror_t *mirror, uint8_t *payload,
                         size_t length) {
    size_t offset = 0;
    while (offset + 4 <= length) {
        const uint32_t nalu_length = be32(payload + offset);
        const size_t prefix = offset;
        offset += 4;
        if (nalu_length == 0 || nalu_length > length - offset) {
            receiver_log("mirror", "mirror invalid NAL length=%" PRIu32
                            " payload=%zu offset=%zu\n",
                    nalu_length, length, offset);
            return -1;
        }
        if (nalu_length >= 1) {
            const uint8_t first = payload[offset];
            if ((first & 0x80u) == 0) {
                mirror->nalu_count++;
                if (mirror->codec == 0) mirror->codec = 264;
            }
        }
        memcpy(payload + prefix, "\0\0\0\1", 4);
        offset += nalu_length;
    }
    if (offset != length) {
        receiver_log("mirror", "mirror trailing NAL bytes=%zu\n", length - offset);
        return -1;
    }
    return 0;
}

static int inspect_codec_packet(airplay_mirror_t *mirror) {
    const uint8_t option_hi = mirror->header[6];
    const uint8_t option_lo = mirror->header[7];
    if (option_hi == 0x1e && option_lo == 0x01) {
        mirror->codec = 265;
    } else if (option_hi == 0x16 && option_lo == 0x01) {
        mirror->codec = 264;
    }
    return mirror->codec;
}

static void save_h264_config(airplay_mirror_t *mirror, const uint8_t *payload,
                             size_t length) {
    if (length < 11) {
        return;
    }
    const size_t sps_length = ((size_t)payload[6] << 8) | payload[7];
    const size_t sps_at = 8;
    if (sps_length == 0 || sps_at + sps_length + 3 > length) {
        return;
    }
    const size_t pps_length_at = sps_at + sps_length + 1;
    const size_t pps_length = ((size_t)payload[pps_length_at] << 8) |
                              payload[pps_length_at + 1];
    const size_t pps_at = pps_length_at + 2;
    if (pps_length == 0 || pps_at + pps_length > length) {
        return;
    }
    uint8_t *config = malloc(sps_length + pps_length + 8);
    if (!config) {
        return;
    }
    memcpy(config, "\0\0\0\1", 4);
    memcpy(config + 4, payload + sps_at, sps_length);
    memcpy(config + 4 + sps_length, "\0\0\0\1", 4);
    memcpy(config + 8 + sps_length, payload + pps_at, pps_length);
    free(mirror->pending_config);
    mirror->pending_config = config;
    mirror->pending_config_length = sps_length + pps_length + 8;
    mirror->codec = 264;
}

static int accept_stream(airplay_mirror_t *mirror) {
    struct sockaddr_in address;
    socklen_t length = sizeof(address);
    int fd = accept(mirror->listen_fd, (struct sockaddr *)&address, &length);
    if (fd < 0) {
        return errno == EAGAIN || errno == EWOULDBLOCK ? 0 : -1;
    }
    (void)make_nonblocking(fd);
    if (mirror->stream_fd >= 0) {
        close(mirror->stream_fd);
    }
    mirror->stream_fd = fd;
    mirror->header_used = 0;
    mirror->payload_used = 0;
    mirror->payload_length = 0;
    mirror->running = 1;
    receiver_log("mirror", "mirror TCP client connected\n");
    return 1;
}

static int receive_stream(airplay_mirror_t *mirror) {
    if (mirror->stream_fd < 0) {
        return 0;
    }
    for (;;) {
        if (mirror->header_used < MIRROR_HEADER_SIZE) {
            ssize_t count = recv(mirror->stream_fd,
                                 mirror->header + mirror->header_used,
                                 MIRROR_HEADER_SIZE - mirror->header_used, 0);
            if (count == 0) {
                close(mirror->stream_fd);
                mirror->stream_fd = -1;
                return 1;
            }
            if (count < 0) {
                return errno == EAGAIN || errno == EWOULDBLOCK ? 0 : -1;
            }
            mirror->header_used += (size_t)count;
            if (mirror->header_used < MIRROR_HEADER_SIZE) {
                continue;
            }
            mirror->payload_length = le32(mirror->header);
            if (mirror->payload_length > MIRROR_MAX_PAYLOAD) {
                receiver_log("mirror", "mirror payload too large=%" PRIu32 "\n",
                        mirror->payload_length);
                close(mirror->stream_fd);
                mirror->stream_fd = -1;
                return -1;
            }
            if (mirror->payload_length > mirror->payload_capacity) {
                uint8_t *next = realloc(mirror->payload, mirror->payload_length);
                if (!next) {
                    return -1;
                }
                mirror->payload = next;
                mirror->payload_capacity = mirror->payload_length;
            }
            mirror->payload_used = 0;
        }

        while (mirror->payload_used < mirror->payload_length) {
            ssize_t count = recv(mirror->stream_fd,
                                 mirror->payload + mirror->payload_used,
                                 mirror->payload_length - mirror->payload_used, 0);
            if (count == 0) {
                close(mirror->stream_fd);
                mirror->stream_fd = -1;
                return 1;
            }
            if (count < 0) {
                return errno == EAGAIN || errno == EWOULDBLOCK ? 0 : -1;
            }
            mirror->payload_used += (size_t)count;
        }

        mirror->packets++;
        mirror->bytes += mirror->payload_length;
        if (mirror->header[4] == 0x00) {
            mirror->video_packets++;
            if (mirror->video_packets == 1 || mirror->video_packets % 300 == 0) {
                receiver_log("mirror", "video packets=%" PRIu64 " last payload=%" PRIu32
                             " bytes", mirror->video_packets, mirror->payload_length);
            }
            decrypt_payload(mirror, mirror->payload, mirror->payload_length);
            if (mirror->video_packets <= 3) {
                receiver_log("mirror", "video #%" PRIu64 " after decrypt: %02x %02x %02x %02x %02x %02x %02x %02x (%" PRIu32 " bytes)",
                             mirror->video_packets, mirror->payload[0], mirror->payload[1],
                             mirror->payload[2], mirror->payload[3], mirror->payload[4],
                             mirror->payload[5], mirror->payload[6], mirror->payload[7],
                             mirror->payload_length);
            }
            if (convert_nalus(mirror, mirror->payload, mirror->payload_length) == 0 &&
                mirror->video_callback) {
                const size_t config_length = mirror->pending_config_length;
                const size_t total = config_length + mirror->payload_length;
                uint8_t *frame = malloc(total);
                if (frame) {
                    if (config_length) {
                        memcpy(frame, mirror->pending_config, config_length);
                    }
                    memcpy(frame + config_length, mirror->payload,
                           mirror->payload_length);
                    mirror->video_callback(mirror->video_callback_opaque, frame,
                                           total, mirror->codec ? mirror->codec : 264,
                                           header_timestamp_us(mirror->header));
                    free(frame);
                    free(mirror->pending_config);
                    mirror->pending_config = NULL;
                    mirror->pending_config_length = 0;
                }
            }
        } else if (mirror->header[4] == 0x01) {
            mirror->codec_packets++;
            (void)inspect_codec_packet(mirror);
            if (mirror->codec == 264) {
                save_h264_config(mirror, mirror->payload, mirror->payload_length);
            }
            receiver_log("mirror", "mirror codec packet option=%02x%02x size=%" PRIu32
                            " codec=H%d\n", mirror->header[6], mirror->header[7],
                    mirror->payload_length, mirror->codec);
        }
        mirror->header_used = 0;
        mirror->payload_used = 0;
        mirror->payload_length = 0;
    }
}

void airplay_mirror_stop_stream(airplay_mirror_t *mirror) {
    if (!mirror || mirror->stream_fd < 0) {
        return;
    }
    close(mirror->stream_fd);
    mirror->stream_fd = -1;
    mirror->header_used = 0;
    mirror->payload_used = 0;
    mirror->payload_length = 0;
}

int airplay_mirror_init(airplay_mirror_t *mirror) {
    if (!mirror) {
        return -1;
    }
    memset(mirror, 0, sizeof(*mirror));
    mirror->listen_fd = -1;
    mirror->stream_fd = -1;
    mirror->codec = 0;
    return airplay_mirror_reopen_listener(mirror);
}

int airplay_mirror_reopen_listener(airplay_mirror_t *mirror) {
    if (!mirror) {
        return -1;
    }
    airplay_mirror_stop_stream(mirror);
    if (mirror->listen_fd >= 0) {
        close(mirror->listen_fd);
        mirror->listen_fd = -1;
    }
    mirror->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (mirror->listen_fd < 0) {
        return -1;
    }
    int reuse = 1;
    (void)setsockopt(mirror->listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(0);
    if (bind(mirror->listen_fd, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(mirror->listen_fd, 2) != 0 || make_nonblocking(mirror->listen_fd) != 0) {
        close(mirror->listen_fd);
        mirror->listen_fd = -1;
        return -1;
    }
    return 0;
}

void airplay_mirror_close(airplay_mirror_t *mirror) {
    if (!mirror) {
        return;
    }
    if (mirror->stream_fd >= 0) {
        close(mirror->stream_fd);
        mirror->stream_fd = -1;
    }
    if (mirror->listen_fd >= 0) {
        close(mirror->listen_fd);
        mirror->listen_fd = -1;
    }
    free(mirror->payload);
    mirror->payload = NULL;
    mirror->payload_capacity = 0;
    free(mirror->pending_config);
    mirror->pending_config = NULL;
    mirror->pending_config_length = 0;
    free(mirror->aes);
    mirror->aes = NULL;
}

void airplay_mirror_set_audio_key(airplay_mirror_t *mirror,
                                  const uint8_t audio_key[16]) {
    if (!mirror || !audio_key) {
        return;
    }
    memcpy(mirror->audio_key, audio_key, sizeof(mirror->audio_key));
    mirror->have_audio_key = 1;
    derive_video_cipher(mirror);
}

int airplay_mirror_start(airplay_mirror_t *mirror, uint64_t stream_connection_id) {
    if (!mirror || mirror->listen_fd < 0) {
        return -1;
    }
    mirror->stream_connection_id = stream_connection_id;
    mirror->have_stream_id = 1;
    derive_video_cipher(mirror);
    mirror->header_used = 0;
    mirror->payload_used = 0;
    mirror->payload_length = 0;
    receiver_log("mirror", "mirror listener ready on TCP/%d streamConnectionID=%" PRIu64
                    " cipher=%s\n", airplay_mirror_port(mirror), stream_connection_id,
            mirror->have_video_cipher ? "ready" : "waiting-for-audio-key");
    return 0;
}

int airplay_mirror_poll(airplay_mirror_t *mirror) {
    if (!mirror || mirror->listen_fd < 0) {
        return -1;
    }
    int result = 0;
    if (mirror->stream_fd < 0) {
        result = accept_stream(mirror);
        if (result < 0) {
            return result;
        }
    }
    int received = receive_stream(mirror);
    return received < 0 ? received : result + received;
}

int airplay_mirror_port(const airplay_mirror_t *mirror) {
    if (!mirror || mirror->listen_fd < 0) {
        return -1;
    }
    struct sockaddr_in address;
    socklen_t length = sizeof(address);
    if (getsockname(mirror->listen_fd, (struct sockaddr *)&address, &length) != 0) {
        return -1;
    }
    return ntohs(address.sin_port);
}

void airplay_mirror_set_video_callback(airplay_mirror_t *mirror,
                                       airplay_mirror_video_callback_t callback,
                                       void *opaque) {
    if (!mirror) {
        return;
    }
    mirror->video_callback = callback;
    mirror->video_callback_opaque = opaque;
}
