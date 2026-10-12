#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct {
    int fd;
    uint32_t address;
    char address_text[16];
    char host_name[64];
    char display_name[64];
    char public_key[65];
    int enabled;
    int pin_required;
} mdns_server_t;

int mdns_server_init(mdns_server_t *server);
void mdns_server_close(mdns_server_t *server);
int mdns_server_poll(mdns_server_t *server, int timeout_ms);
int mdns_server_announce(mdns_server_t *server);
int mdns_server_set_enabled(mdns_server_t *server, int enabled);
int mdns_server_set_name(mdns_server_t *server, const char *name);
int mdns_server_set_public_key(mdns_server_t *server, const char *hex);
int mdns_server_set_pin_required(mdns_server_t *server, int required);
const char *mdns_server_name(const mdns_server_t *server);
