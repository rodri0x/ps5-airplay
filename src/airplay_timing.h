#pragma once

#include <stdint.h>
#include <netinet/in.h>

typedef struct {
    int fd;
    struct sockaddr_in peer;
    int peer_valid;
    uint16_t peer_port;
    uint64_t next_probe_ms;
    uint64_t packets;
} airplay_timing_t;

int airplay_timing_init(airplay_timing_t *timing);
void airplay_timing_close(airplay_timing_t *timing);
void airplay_timing_set_peer(airplay_timing_t *timing,
                             const struct sockaddr_in *peer,
                             uint16_t remote_port);
void airplay_timing_clear_peer(airplay_timing_t *timing);
int airplay_timing_poll(airplay_timing_t *timing, uint64_t now_ms);
int airplay_timing_port(const airplay_timing_t *timing);
