/* Host harness for tests/test_srp.py: drives src/airplay_srp.c over stdin/stdout. */
#include "airplay_srp.h"

#include <stdio.h>
#include <string.h>

static size_t from_hex(const char *hex, uint8_t *out, size_t capacity) {
    size_t length = strlen(hex) / 2;
    if (length > capacity) length = capacity;
    for (size_t i = 0; i < length; i++) {
        unsigned value = 0;
        sscanf(hex + 2 * i, "%2x", &value);
        out[i] = (uint8_t)value;
    }
    return length;
}

static void print_hex(const uint8_t *data, size_t length) {
    for (size_t i = 0; i < length; i++) printf("%02x", data[i]);
}

int main(void) {
    static char line[8192], a[4096], b[4096], c[4096];
    airplay_srp_t session;
    memset(&session, 0, sizeof(session));
    while (fgets(line, sizeof(line), stdin)) {
        char command[32];
        a[0] = b[0] = c[0] = '\0';
        if (sscanf(line, "%31s %4095s %4095s %4095s", command, a, b, c) < 1) continue;
        if (!strcmp(command, "sha1")) {
            uint8_t data[1024], digest[20];
            size_t n = from_hex(a, data, sizeof(data));
            airplay_sha1(data, n, digest);
            print_hex(digest, 20);
        } else if (!strcmp(command, "gcm")) { /* gcm key nonce plain */
            uint8_t key[16], nonce[16], plain[512], cipher[512], tag[16], back[512];
            from_hex(a, key, 16);
            from_hex(b, nonce, 16);
            size_t n = from_hex(c, plain, sizeof(plain));
            airplay_gcm_encrypt(key, nonce, plain, n, cipher, tag);
            int ok = airplay_gcm_decrypt(key, nonce, cipher, n, tag, back);
            tag[0] ^= 1;
            int bad = airplay_gcm_decrypt(key, nonce, cipher, n, tag, back);
            tag[0] ^= 1;
            print_hex(cipher, n); printf(" "); print_hex(tag, 16);
            printf(" %d %d", ok, bad);
        } else if (!strcmp(command, "start")) { /* start user pin random */
            uint8_t random[48];
            from_hex(c, random, sizeof(random));
            if (airplay_srp_start(&session, a, b, random) != 0) { printf("fail"); }
            else { print_hex(session.salt, 16); printf(" "); print_hex(session.public_key, session.public_key_length); }
        } else if (!strcmp(command, "prove")) { /* prove A M1 */
            uint8_t pub[300], proof[20], reply[20];
            size_t n = from_hex(a, pub, sizeof(pub));
            from_hex(b, proof, 20);
            if (airplay_srp_prove(&session, pub, n, proof, reply) != 0) { printf("fail"); }
            else { printf("ok "); print_hex(reply, 20); printf(" "); print_hex(session.session_key, 40); }
        } else if (!strcmp(command, "keys")) { /* keys epk tag server_pk */
            uint8_t epk[32], tag[16], server_pk[32], client_pk[32], out_epk[32], out_tag[16];
            from_hex(a, epk, 32); from_hex(b, tag, 16); from_hex(c, server_pk, 32);
            if (airplay_srp_exchange_keys(&session, epk, tag, server_pk, client_pk, out_epk, out_tag) != 0) { printf("fail"); }
            else { printf("ok "); print_hex(client_pk, 32); printf(" "); print_hex(out_epk, 32); printf(" "); print_hex(out_tag, 16); }
        }
        printf("\n");
        fflush(stdout);
    }
    return 0;
}
