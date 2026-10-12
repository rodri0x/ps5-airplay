#pragma once

#include <stddef.h>
#include <stdint.h>

typedef void (*airplay_audio_callback_t)(void *opaque, const uint8_t *payload,
                                         size_t length, uint8_t codec,
                                         uint32_t timestamp);

typedef struct {
    int data_fd;
    int control_fd;
    uint8_t key[16];
    uint8_t iv[16];
    int have_crypto;
    uint8_t codec;
    uint16_t samples_per_frame;
    uint8_t packet[65536];
    uint8_t decoded[65536];
    uint64_t packets;
    uint64_t bytes;
    uint32_t last_timestamp;
    int have_timestamp;
    uint64_t duplicates;
    uint64_t last_arrival_ms;
    uint64_t longest_gap_ms;
    unsigned int gap_counts[6];
    airplay_audio_callback_t callback;
    void *callback_opaque;
} airplay_audio_t;

int airplay_audio_init(airplay_audio_t *audio);
void airplay_audio_close(airplay_audio_t *audio);
void airplay_audio_set_crypto(airplay_audio_t *audio, const uint8_t key[16],
                              const uint8_t iv[16]);
void airplay_audio_set_format(airplay_audio_t *audio, uint8_t codec,
                              uint16_t samples_per_frame);
void airplay_audio_set_callback(airplay_audio_t *audio,
                                airplay_audio_callback_t callback, void *opaque);
int airplay_audio_poll(airplay_audio_t *audio);
int airplay_audio_data_port(const airplay_audio_t *audio);
int airplay_audio_control_port(const airplay_audio_t *audio);
