#include "airplay_crypto.h"

#include "crypto/aes.h"

#include <stdlib.h>
#include <string.h>

void airplay_aes_ctr_init(airplay_aes_ctr_t *ctx, const uint8_t key[16],
                          const uint8_t iv[16]) {
    AES_init_ctx_iv(&ctx->aes, key, iv);
    ctx->block_offset = 0;
}

void airplay_aes_ctr_crypt(airplay_aes_ctr_t *ctx, const uint8_t *input,
                           uint8_t *output, size_t length) {
    if (!ctx || !input || !output || length == 0) {
        return;
    }
    if (input == output) {
        AES_CTR_xcrypt_buffer(&ctx->aes, output, length);
        ctx->block_offset = (ctx->block_offset + length) & 15u;
        return;
    }
    uint8_t scratch[16];
    size_t done = 0;
    while (done < length) {
        const size_t chunk = length - done < sizeof(scratch) ? length - done : sizeof(scratch);
        memcpy(scratch, input + done, chunk);
        if (chunk < sizeof(scratch)) {
            memset(scratch + chunk, 0, sizeof(scratch) - chunk);
        }
        AES_CTR_xcrypt_buffer(&ctx->aes, scratch, sizeof(scratch));
        memcpy(output + done, scratch, chunk);
        done += chunk;
        ctx->block_offset = 0;
    }
}

void airplay_aes_ctr_start_fresh_block(airplay_aes_ctr_t *ctx) {
    if (!ctx || ctx->block_offset == 0) {
        return;
    }
    uint8_t discard[16] = {0};
    const size_t remaining = 16 - ctx->block_offset;
    AES_CTR_xcrypt_buffer(&ctx->aes, discard, remaining);
    ctx->block_offset = 0;
}

#include "playfair/playfair.h"

struct airplay_fairplay {
    uint8_t key_message[164];
    unsigned int key_message_length;
};

static const uint8_t fairplay_reply[4][142] = {
#include "fairplay_replies.inc"
};

static const uint8_t fairplay_header[12] = {
    0x46, 0x50, 0x4c, 0x59, 0x03, 0x01, 0x04, 0x00,
    0x00, 0x00, 0x00, 0x14
};

airplay_fairplay_t *airplay_fairplay_create(void) {
    return calloc(1, sizeof(airplay_fairplay_t));
}

void airplay_fairplay_destroy(airplay_fairplay_t *fairplay) {
    free(fairplay);
}

int airplay_fairplay_setup(airplay_fairplay_t *fairplay,
                           const uint8_t request[16], uint8_t response[142]) {
    if (!fairplay || !request || !response || request[4] != 0x03 || request[14] > 3) {
        return -1;
    }
    memcpy(response, fairplay_reply[request[14]], 142);
    fairplay->key_message_length = 0;
    return 0;
}

int airplay_fairplay_handshake(airplay_fairplay_t *fairplay,
                               const uint8_t request[164], uint8_t response[32]) {
    if (!fairplay || !request || !response || request[4] != 0x03) {
        return -1;
    }
    memcpy(fairplay->key_message, request, 164);
    fairplay->key_message_length = 164;
    memcpy(response, fairplay_header, sizeof(fairplay_header));
    memcpy(response + sizeof(fairplay_header), request + 144, 20);
    return 0;
}

int airplay_fairplay_decrypt(airplay_fairplay_t *fairplay,
                             const uint8_t input[72], uint8_t output[16]) {
    if (!fairplay || !input || !output || fairplay->key_message_length != 164) {
        return -1;
    }
    playfair_decrypt(fairplay->key_message, (uint8_t *)input, output);
    return 0;
}
