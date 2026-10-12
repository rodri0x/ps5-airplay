#pragma once

#include "airplay_crypto.h"

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t secret_key[64];
    uint8_t public_key[32];
} airplay_identity_t;

typedef struct {
    int setup_done;
    int verify_started;
    int verified;
    uint8_t client_ed_public[32];
    uint8_t client_ecdh_public[32];
    uint8_t server_ecdh_public[32];
    uint8_t shared_secret[32];
    airplay_aes_ctr_t ctr;
} airplay_pairing_session_t;

#define AIRPLAY_PAIRED_MAX 16

typedef struct {
    char passcode[8];
    uint8_t keys[AIRPLAY_PAIRED_MAX][32];
    size_t count;
} airplay_paired_t;

void airplay_paired_load(airplay_paired_t *paired, const char *passcode, const char *path);
int airplay_paired_contains(const airplay_paired_t *paired, const uint8_t key[32]);
void airplay_paired_add(airplay_paired_t *paired, const uint8_t key[32], const char *path);

int airplay_random_bytes(uint8_t *out, size_t length);

int airplay_identity_load(airplay_identity_t *identity, const char *path);
void airplay_identity_public_hex(const airplay_identity_t *identity, char out[65]);

void airplay_pairing_session_reset(airplay_pairing_session_t *session);
int airplay_pairing_setup(const airplay_identity_t *identity,
                          airplay_pairing_session_t *session,
                          const uint8_t request[32], uint8_t response[32]);
int airplay_pairing_verify_start(const airplay_identity_t *identity,
                                 airplay_pairing_session_t *session,
                                 const uint8_t request[68], uint8_t response[96]);
int airplay_pairing_verify_finish(airplay_pairing_session_t *session,
                                  const uint8_t request[68]);
int airplay_pairing_hash_key(const airplay_pairing_session_t *session,
                             uint8_t key[16]);
