#pragma once

#include <stddef.h>
#include <stdint.h>

#include "crypto/aes.h"

typedef void (*airplay_mirror_video_callback_t)(void *opaque,
                                                const uint8_t *annex_b,
                                                size_t length, int codec,
        uint64_t timestamp_us);

typedef struct {
    int listen_fd;
    int stream_fd;
    uint8_t header[128];
    size_t header_used;
    uint8_t *payload;
    size_t payload_capacity;
    uint8_t *pending_config;
    size_t pending_config_length;
    size_t payload_used;
    uint32_t payload_length;
    uint8_t audio_key[16];
    int have_audio_key;
    uint64_t stream_connection_id;
    int have_stream_id;
    uint8_t video_key[16];
    uint8_t video_iv[16];
    int have_video_cipher;
    struct AES_ctx *aes;
    uint8_t keystream[16];
    size_t leftover_keystream;
    uint64_t packets;
    uint64_t video_packets;
    uint64_t codec_packets;
    uint64_t nalu_count;
    uint64_t bytes;
    int codec;
    int running;
    airplay_mirror_video_callback_t video_callback;
    void *video_callback_opaque;
} airplay_mirror_t;

int airplay_mirror_init(airplay_mirror_t *mirror);
int airplay_mirror_reopen_listener(airplay_mirror_t *mirror);
void airplay_mirror_close(airplay_mirror_t *mirror);
int airplay_mirror_start(airplay_mirror_t *mirror, uint64_t stream_connection_id);
void airplay_mirror_set_audio_key(airplay_mirror_t *mirror,
                                  const uint8_t audio_key[16]);
int airplay_mirror_poll(airplay_mirror_t *mirror);
void airplay_mirror_stop_stream(airplay_mirror_t *mirror);
int airplay_mirror_port(const airplay_mirror_t *mirror);
void airplay_mirror_set_video_callback(airplay_mirror_t *mirror,
                                       airplay_mirror_video_callback_t callback,
                                       void *opaque);
