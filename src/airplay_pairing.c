#include "airplay_pairing.h"
#include "receiver_log.h"

#include "tweetnacl.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static const char pair_verify_key_salt[] = "Pair-Verify-AES-Key";
static const char pair_verify_iv_salt[] = "Pair-Verify-AES-IV";

static int entropy_failed = 0;

static int read_random(uint8_t *buffer, size_t length) {
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) {
        return -1;
    }
    size_t done = 0;
    while (done < length) {
        ssize_t count = read(fd, buffer + done, length - done);
        if (count <= 0) {
            close(fd);
            return -1;
        }
        done += (size_t)count;
    }
    close(fd);
    return 0;
}

int airplay_random_bytes(uint8_t *out, size_t length) {
    return read_random(out, length);
}

static void paired_save(const airplay_paired_t *paired, const char *path) {
    FILE *file = fopen(path, "wb");
    if (!file) {
        return;
    }
    (void)fwrite(paired->passcode, 1, sizeof(paired->passcode), file);
    (void)fwrite(paired->keys, 32, paired->count, file);
    (void)fclose(file);
}

void airplay_paired_load(airplay_paired_t *paired, const char *passcode, const char *path) {
    airplay_paired_t stored;
    memset(&stored, 0, sizeof(stored));
    FILE *file = fopen(path, "rb");
    const int had_file = file != NULL;
    if (file) {
        if (fread(stored.passcode, 1, sizeof(stored.passcode), file) == sizeof(stored.passcode)) {
            stored.count = fread(stored.keys, 32, AIRPLAY_PAIRED_MAX, file);
        }
        (void)fclose(file);
    }
    stored.passcode[sizeof(stored.passcode) - 1] = '\0';
    memset(paired, 0, sizeof(*paired));
    snprintf(paired->passcode, sizeof(paired->passcode), "%s", passcode ? passcode : "");
    if (paired->passcode[0] != '\0' && strcmp(stored.passcode, paired->passcode) == 0) {
        memcpy(paired->keys, stored.keys, sizeof(paired->keys));
        paired->count = stored.count;
        return;
    }
    if (had_file) {
        paired_save(paired, path);
    }
}

int airplay_paired_contains(const airplay_paired_t *paired, const uint8_t key[32]) {
    for (size_t i = 0; i < paired->count; i++) {
        if (memcmp(paired->keys[i], key, 32) == 0) {
            return 1;
        }
    }
    return 0;
}

void airplay_paired_add(airplay_paired_t *paired, const uint8_t key[32], const char *path) {
    if (airplay_paired_contains(paired, key)) {
        return;
    }
    if (paired->count == AIRPLAY_PAIRED_MAX) {
        memmove(paired->keys[0], paired->keys[1], (AIRPLAY_PAIRED_MAX - 1) * 32);
        paired->count--;
    }
    memcpy(paired->keys[paired->count++], key, 32);
    paired_save(paired, path);
}

void randombytes(unsigned char *buffer, unsigned long long length) {
    if (read_random(buffer, (size_t)length) != 0) {
        memset(buffer, 0, (size_t)length);
        entropy_failed = 1;
    }
}

static void derive_pair_verify_block(const uint8_t *salt, size_t salt_length,
                                     const uint8_t secret[32], uint8_t out[16]) {
    uint8_t input[64];
    uint8_t digest[64];
    memcpy(input, salt, salt_length);
    memcpy(input + salt_length, secret, 32);
    crypto_hash(digest, input, (unsigned long long)(salt_length + 32));
    memcpy(out, digest, 16);
    memset(digest, 0, sizeof(digest));
    memset(input, 0, sizeof(input));
}

static void derive_pair_verify_key_iv(const uint8_t secret[32], uint8_t key[16],
                                      uint8_t iv[16]) {
    derive_pair_verify_block((const uint8_t *)pair_verify_key_salt,
                             sizeof(pair_verify_key_salt) - 1, secret, key);
    derive_pair_verify_block((const uint8_t *)pair_verify_iv_salt,
                             sizeof(pair_verify_iv_salt) - 1, secret, iv);
}

int airplay_identity_load(airplay_identity_t *identity, const char *path) {
    memset(identity, 0, sizeof(*identity));
    FILE *file = fopen(path, "rb");
    if (file) {
        size_t count = fread(identity->secret_key, 1, sizeof(identity->secret_key), file);
        fclose(file);
        if (count == sizeof(identity->secret_key)) {
            memcpy(identity->public_key, identity->secret_key + 32, 32);
            return 0;
        }
    }

    entropy_failed = 0;
    if (crypto_sign_keypair(identity->public_key, identity->secret_key) != 0 ||
        entropy_failed) {
        memset(identity, 0, sizeof(*identity));
        return -1;
    }
    file = fopen(path, "wb");
    if (!file) {
        receiver_log("pair", "cannot store identity in %s; pk changes on restart\n",
                path);
        return 0;
    }
    size_t written = fwrite(identity->secret_key, 1, sizeof(identity->secret_key), file);
    if (fclose(file) != 0 || written != sizeof(identity->secret_key)) {
        receiver_log("pair", "writing identity to %s failed\n", path);
    }
    return 0;
}

void airplay_identity_public_hex(const airplay_identity_t *identity, char out[65]) {
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < 32; i++) {
        out[2 * i] = digits[identity->public_key[i] >> 4];
        out[2 * i + 1] = digits[identity->public_key[i] & 0x0f];
    }
    out[64] = '\0';
}

void airplay_pairing_session_reset(airplay_pairing_session_t *session) {
    memset(session, 0, sizeof(*session));
}

int airplay_pairing_setup(const airplay_identity_t *identity,
                          airplay_pairing_session_t *session,
                          const uint8_t request[32], uint8_t response[32]) {
    memcpy(session->client_ed_public, request, 32);
    memcpy(response, identity->public_key, 32);
    session->setup_done = 1;
    return 0;
}

int airplay_pairing_verify_start(const airplay_identity_t *identity,
                                 airplay_pairing_session_t *session,
                                 const uint8_t request[68], uint8_t response[96]) {
    uint8_t server_secret[32];
    uint8_t message[64];
    uint8_t signed_message[128];
    unsigned long long signed_length = 0;
    uint8_t key[16];
    uint8_t iv[16];

    if (request[0] != 1 || read_random(server_secret, sizeof(server_secret)) != 0) {
        return -1;
    }
    memcpy(session->client_ecdh_public, request + 4, 32);
    memcpy(session->client_ed_public, request + 36, 32);
    crypto_scalarmult_base(session->server_ecdh_public, server_secret);
    crypto_scalarmult(session->shared_secret, server_secret, session->client_ecdh_public);
    memset(server_secret, 0, sizeof(server_secret));

    memcpy(message, session->server_ecdh_public, 32);
    memcpy(message + 32, session->client_ecdh_public, 32);
    if (crypto_sign(signed_message, &signed_length, message, sizeof(message),
                    identity->secret_key) != 0) {
        return -1;
    }

    derive_pair_verify_key_iv(session->shared_secret, key, iv);
    airplay_aes_ctr_init(&session->ctr, key, iv);
    memcpy(response, session->server_ecdh_public, 32);
    airplay_aes_ctr_crypt(&session->ctr, signed_message, response + 32, 64);
    session->verify_started = 1;

    memset(key, 0, sizeof(key));
    memset(iv, 0, sizeof(iv));
    return 0;
}

int airplay_pairing_verify_finish(airplay_pairing_session_t *session,
                                  const uint8_t request[68]) {
    uint8_t signed_message[128];
    uint8_t opened[128];
    unsigned long long opened_length = 0;

    if (!session->verify_started || request[0] != 0) {
        return -1;
    }
    airplay_aes_ctr_crypt(&session->ctr, request + 4, signed_message, 64);
    memcpy(signed_message + 64, session->client_ecdh_public, 32);
    memcpy(signed_message + 96, session->server_ecdh_public, 32);
    if (crypto_sign_open(opened, &opened_length, signed_message, sizeof(signed_message),
                         session->client_ed_public) != 0) {
        return -2;
    }
    session->verified = 1;
    return 0;
}

int airplay_pairing_hash_key(const airplay_pairing_session_t *session,
                             uint8_t key[16]) {
    if (!session->verify_started) {
        return 0;
    }
    uint8_t input[48];
    uint8_t digest[64];
    memcpy(input, key, 16);
    memcpy(input + 16, session->shared_secret, 32);
    crypto_hash(digest, input, sizeof(input));
    memcpy(key, digest, 16);
    memset(digest, 0, sizeof(digest));
    memset(input, 0, sizeof(input));
    return 1;
}
