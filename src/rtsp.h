#pragma once

#include "airplay_crypto.h"
#include "airplay_pairing.h"
#include "airplay_srp.h"
#include "airplay_audio.h"
#include "airplay_mirror.h"
#include "airplay_timing.h"

#include <stddef.h>
#include <stdint.h>

#define RTSP_RX_CAPACITY (128u * 1024u)

typedef void (*rtsp_mirror_end_callback_t)(void *opaque);

typedef struct {
    int listen_fd;
    int client_fd;
    unsigned int connections;
    int enabled;
    uint8_t rx[RTSP_RX_CAPACITY];
    size_t rx_used;
    airplay_fairplay_t *fairplay;
    airplay_mirror_t mirror;
    airplay_audio_t audio;
    airplay_timing_t timing;
    struct sockaddr_in remote_address;
    int remote_address_valid;
    int setup_keys_ready;
    int stream_ready;
    const airplay_identity_t *identity;
    airplay_pairing_session_t pairing;
    rtsp_mirror_end_callback_t mirror_end_callback;
    void *mirror_end_opaque;
    char name[64];
    char passcode[8];
    airplay_srp_t srp;
    airplay_paired_t paired;
    unsigned int wrong_codes;
    uint64_t pairing_paused_until_ms;
    uint64_t last_request_ms;
    uint64_t next_listener_check_ms;
    int self_check_pending;
    unsigned int listener_generation;
} rtsp_server_t;

typedef void (*rtsp_video_callback_t)(void *opaque, const uint8_t *annex_b,
                                      size_t length, int codec,
        uint64_t timestamp_us);
typedef void (*rtsp_audio_callback_t)(void *opaque, const uint8_t *payload,
                                      size_t length, uint8_t codec,
                                      uint32_t timestamp);

int rtsp_server_init(rtsp_server_t *server);
void rtsp_server_set_identity(rtsp_server_t *server,
                              const airplay_identity_t *identity);
void rtsp_server_close(rtsp_server_t *server);
int rtsp_server_poll(rtsp_server_t *server, int timeout_ms);
int rtsp_server_set_enabled(rtsp_server_t *server, int enabled);
void rtsp_server_set_name(rtsp_server_t *server, const char *name);
void rtsp_server_set_passcode(rtsp_server_t *server, const char *passcode);
void rtsp_server_set_video_callback(rtsp_server_t *server,
                                    rtsp_video_callback_t callback, void *opaque);
void rtsp_server_set_mirror_end_callback(rtsp_server_t *server,
                                         rtsp_mirror_end_callback_t callback,
                                         void *opaque);
void rtsp_server_set_audio_callback(rtsp_server_t *server,
                                    rtsp_audio_callback_t callback, void *opaque);
