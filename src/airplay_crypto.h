#pragma once

#include <stddef.h>
#include <stdint.h>

#include "crypto/aes.h"

typedef struct {
    struct AES_ctx aes;
    size_t block_offset;
} airplay_aes_ctr_t;

void airplay_aes_ctr_init(airplay_aes_ctr_t *ctx, const uint8_t key[16],
                          const uint8_t iv[16]);
void airplay_aes_ctr_crypt(airplay_aes_ctr_t *ctx, const uint8_t *input,
                           uint8_t *output, size_t length);
void airplay_aes_ctr_start_fresh_block(airplay_aes_ctr_t *ctx);

typedef struct airplay_fairplay airplay_fairplay_t;

airplay_fairplay_t *airplay_fairplay_create(void);
void airplay_fairplay_destroy(airplay_fairplay_t *fairplay);
int airplay_fairplay_setup(airplay_fairplay_t *fairplay,
                           const uint8_t request[16], uint8_t response[142]);
int airplay_fairplay_handshake(airplay_fairplay_t *fairplay,
                               const uint8_t request[164], uint8_t response[32]);
int airplay_fairplay_decrypt(airplay_fairplay_t *fairplay,
                             const uint8_t input[72], uint8_t output[16]);
