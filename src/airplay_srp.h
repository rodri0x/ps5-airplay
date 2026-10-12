#pragma once

#include <stddef.h>
#include <stdint.h>

#define AIRPLAY_SRP_SALT_SIZE 16
#define AIRPLAY_SRP_PRIVATE_SIZE 32
#define AIRPLAY_SRP_GROUP_SIZE 256
#define AIRPLAY_SRP_PROOF_SIZE 20
#define AIRPLAY_SRP_KEY_SIZE 40

typedef struct {
    char username[64];
    uint8_t salt[AIRPLAY_SRP_SALT_SIZE];
    uint8_t private_key[AIRPLAY_SRP_PRIVATE_SIZE];
    uint8_t verifier[AIRPLAY_SRP_GROUP_SIZE];
    uint8_t public_key[AIRPLAY_SRP_GROUP_SIZE];
    size_t public_key_length;
    uint8_t session_key[AIRPLAY_SRP_KEY_SIZE];
    int started;
    int proved;
} airplay_srp_t;

int airplay_srp_start(airplay_srp_t *session, const char *username, const char *pin,
                      const uint8_t *random);

int airplay_srp_prove(airplay_srp_t *session, const uint8_t *client_public,
                      size_t client_public_length,
                      const uint8_t client_proof[AIRPLAY_SRP_PROOF_SIZE],
                      uint8_t server_proof[AIRPLAY_SRP_PROOF_SIZE]);

int airplay_srp_exchange_keys(airplay_srp_t *session, const uint8_t client_epk[32],
                              const uint8_t client_tag[16],
                              const uint8_t server_public_key[32],
                              uint8_t client_public_key[32], uint8_t server_epk[32],
                              uint8_t server_tag[16]);

void airplay_sha1(const uint8_t *data, size_t length, uint8_t digest[20]);
void airplay_gcm_encrypt(const uint8_t key[16], const uint8_t nonce[16],
                         const uint8_t *plain, size_t length, uint8_t *cipher,
                         uint8_t tag[16]);
int airplay_gcm_decrypt(const uint8_t key[16], const uint8_t nonce[16],
                        const uint8_t *cipher, size_t length, const uint8_t tag[16],
                        uint8_t *plain);
