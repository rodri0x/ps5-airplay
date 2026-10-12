#pragma once

#include "mdns.h"
#include "rtsp.h"

#include <stddef.h>

typedef struct {
    char name[64];
    char passcode[8];
    char mode[16];
    int enabled;
} airplay_settings_t;

typedef struct {
    int listen_fd;
    airplay_settings_t *settings;
    mdns_server_t *mdns;
    rtsp_server_t *rtsp;
    unsigned int listener_generation;
} control_server_t;

void airplay_settings_defaults(airplay_settings_t *settings);
void airplay_settings_load(airplay_settings_t *settings);
int airplay_settings_save(const airplay_settings_t *settings);

int control_server_init(control_server_t *server, airplay_settings_t *settings,
                        mdns_server_t *mdns, rtsp_server_t *rtsp);
void control_server_close(control_server_t *server);
int control_server_poll(control_server_t *server, int timeout_ms);
