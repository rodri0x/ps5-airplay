/* AirPlay PIN pairing (POST /pair-setup-pin), receiver side: SRP-6a in Apple's
 * variant (SHA-1, RFC 5054 2048-bit group, 16-byte salt, 40-byte session key),
 * then the Ed25519 public keys exchanged under AES-128-GCM with a 16-byte
 * nonce.  Numbers are hashed without leading zero bytes unless padded. */

#include "airplay_srp.h"

#include "crypto/aes.h"
#include "tweetnacl.h"

#include <string.h>

typedef struct {
    uint32_t state[5];
    uint64_t length;
    uint8_t block[64];
    size_t used;
} sha1_t;

static uint32_t rotl32(uint32_t value, int bits) {
    return (value << bits) | (value >> (32 - bits));
}

static void sha1_block(sha1_t *sha, const uint8_t block[64]) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[i * 4] << 24) | ((uint32_t)block[i * 4 + 1] << 16) |
               ((uint32_t)block[i * 4 + 2] << 8) | block[i * 4 + 3];
    }
    for (int i = 16; i < 80; i++) {
        w[i] = rotl32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }
    uint32_t a = sha->state[0], b = sha->state[1], c = sha->state[2];
    uint32_t d = sha->state[3], e = sha->state[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) {
            f = (b & c) | (~b & d);
            k = 0x5a827999u;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ed9eba1u;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8f1bbcdcu;
        } else {
            f = b ^ c ^ d;
            k = 0xca62c1d6u;
        }
        const uint32_t next = rotl32(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = rotl32(b, 30);
        b = a;
        a = next;
    }
    sha->state[0] += a;
    sha->state[1] += b;
    sha->state[2] += c;
    sha->state[3] += d;
    sha->state[4] += e;
}

static void sha1_init(sha1_t *sha) {
    sha->state[0] = 0x67452301u;
    sha->state[1] = 0xefcdab89u;
    sha->state[2] = 0x98badcfeu;
    sha->state[3] = 0x10325476u;
    sha->state[4] = 0xc3d2e1f0u;
    sha->length = 0;
    sha->used = 0;
}

static void sha1_update(sha1_t *sha, const void *data, size_t length) {
    const uint8_t *bytes = data;
    sha->length += length;
    while (length > 0) {
        size_t take = sizeof(sha->block) - sha->used;
        if (take > length) {
            take = length;
        }
        memcpy(sha->block + sha->used, bytes, take);
        sha->used += take;
        bytes += take;
        length -= take;
        if (sha->used == sizeof(sha->block)) {
            sha1_block(sha, sha->block);
            sha->used = 0;
        }
    }
}

static void sha1_final(sha1_t *sha, uint8_t digest[20]) {
    const uint64_t bits = sha->length * 8;
    const uint8_t one = 0x80, zero = 0;
    sha1_update(sha, &one, 1);
    while (sha->used != 56) {
        sha1_update(sha, &zero, 1);
    }
    uint8_t size[8];
    for (int i = 0; i < 8; i++) {
        size[i] = (uint8_t)(bits >> (56 - 8 * i));
    }
    sha1_update(sha, size, sizeof(size));
    for (int i = 0; i < 5; i++) {
        digest[i * 4] = (uint8_t)(sha->state[i] >> 24);
        digest[i * 4 + 1] = (uint8_t)(sha->state[i] >> 16);
        digest[i * 4 + 2] = (uint8_t)(sha->state[i] >> 8);
        digest[i * 4 + 3] = (uint8_t)sha->state[i];
    }
}

void airplay_sha1(const uint8_t *data, size_t length, uint8_t digest[20]) {
    sha1_t sha;
    sha1_init(&sha);
    sha1_update(&sha, data, length);
    sha1_final(&sha, digest);
}

#define LIMBS 64

typedef uint32_t bn_t[LIMBS];

static const char prime_hex[] =
    "AC6BDB41324A9A9BF166DE5E1389582FAF72B6651987EE07FC3192943DB56050A37329CBB4"
    "A099ED8193E0757767A13DD52312AB4B03310DCD7F48A9DA04FD50E8083969EDB767B0CF60"
    "95179A163AB3661A05FBD5FAAAE82918A9962F0B93B855F97993EC975EEAA80D740ADBF4FF"
    "747359D041D5C33EA71D281E446B14773BCA97B43A23FB801676BD207A436C6481F1D2B907"
    "8717461A5B9D32E688F87748544523B524B0D57D5EA77A2775D2ECFA032CFBDBF52FB37861"
    "60279004E57AE6AF874E7303CE53299CCC041C7BC308D82A5698F3A8D0C38271AE35F8E9DB"
    "FBB694B5C803D89F7AE435DE236D525F54759B65E372FCD68EF20FA7111F9E4AFF73";

static uint8_t prime_bytes[AIRPLAY_SRP_GROUP_SIZE];
static bn_t prime;
static bn_t r_squared;
static bn_t bn_one;
static uint32_t prime_inverse;
static int group_ready;

static void bn_from_be(bn_t out, const uint8_t *bytes, size_t length) {
    memset(out, 0, sizeof(bn_t));
    for (size_t i = 0; i < length && i < LIMBS * 4; i++) {
        out[i / 4] |= (uint32_t)bytes[length - 1 - i] << (8 * (i % 4));
    }
}

static void bn_to_be(const bn_t value, uint8_t out[AIRPLAY_SRP_GROUP_SIZE]) {
    for (size_t i = 0; i < AIRPLAY_SRP_GROUP_SIZE; i++) {
        out[AIRPLAY_SRP_GROUP_SIZE - 1 - i] = (uint8_t)(value[i / 4] >> (8 * (i % 4)));
    }
}

static int bn_compare(const bn_t a, const bn_t b) {
    for (int i = LIMBS - 1; i >= 0; i--) {
        if (a[i] != b[i]) {
            return a[i] > b[i] ? 1 : -1;
        }
    }
    return 0;
}

static int bn_is_zero(const bn_t a) {
    uint32_t any = 0;
    for (int i = 0; i < LIMBS; i++) {
        any |= a[i];
    }
    return any == 0;
}

static void bn_subtract(bn_t a, const bn_t b) {
    uint64_t borrow = 0;
    for (int i = 0; i < LIMBS; i++) {
        const uint64_t difference = (uint64_t)a[i] - b[i] - borrow;
        a[i] = (uint32_t)difference;
        borrow = (difference >> 32) & 1;
    }
}

/* out = a * b / 2^2048 mod prime (Montgomery, CIOS). */
static void mont_multiply(bn_t out, const bn_t a, const bn_t b) {
    uint32_t t[LIMBS + 2];
    memset(t, 0, sizeof(t));
    for (int i = 0; i < LIMBS; i++) {
        uint64_t carry = 0;
        for (int j = 0; j < LIMBS; j++) {
            const uint64_t sum = (uint64_t)t[j] + (uint64_t)a[j] * b[i] + carry;
            t[j] = (uint32_t)sum;
            carry = sum >> 32;
        }
        uint64_t sum = (uint64_t)t[LIMBS] + carry;
        t[LIMBS] = (uint32_t)sum;
        t[LIMBS + 1] = (uint32_t)(sum >> 32);

        const uint32_t m = t[0] * prime_inverse;
        sum = (uint64_t)t[0] + (uint64_t)m * prime[0];
        carry = sum >> 32;
        for (int j = 1; j < LIMBS; j++) {
            sum = (uint64_t)t[j] + (uint64_t)m * prime[j] + carry;
            t[j - 1] = (uint32_t)sum;
            carry = sum >> 32;
        }
        sum = (uint64_t)t[LIMBS] + carry;
        t[LIMBS - 1] = (uint32_t)sum;
        t[LIMBS] = t[LIMBS + 1] + (uint32_t)(sum >> 32);
    }
    if (t[LIMBS] != 0 || bn_compare(t, prime) >= 0) {
        bn_subtract(t, prime);
    }
    memcpy(out, t, sizeof(bn_t));
}

static void group_init(void) {
    if (group_ready) {
        return;
    }
    for (size_t i = 0; i < AIRPLAY_SRP_GROUP_SIZE; i++) {
        uint8_t byte = 0;
        for (int nibble = 0; nibble < 2; nibble++) {
            const char c = prime_hex[i * 2 + (size_t)nibble];
            byte = (uint8_t)(byte << 4) | (uint8_t)(c <= '9' ? c - '0' : c - 'A' + 10);
        }
        prime_bytes[i] = byte;
    }
    bn_from_be(prime, prime_bytes, sizeof(prime_bytes));

    uint32_t inverse = 1;
    for (int i = 0; i < 6; i++) {
        inverse *= 2u - prime[0] * inverse;
    }
    prime_inverse = 0u - inverse;

    memset(r_squared, 0, sizeof(r_squared));
    r_squared[0] = 1;
    for (int bit = 0; bit < 2 * LIMBS * 32; bit++) {
        uint32_t carry = 0;
        for (int i = 0; i < LIMBS; i++) {
            const uint32_t next = r_squared[i] >> 31;
            r_squared[i] = (r_squared[i] << 1) | carry;
            carry = next;
        }
        if (carry || bn_compare(r_squared, prime) >= 0) {
            bn_subtract(r_squared, prime);
        }
    }
    memset(bn_one, 0, sizeof(bn_one));
    bn_one[0] = 1;
    group_ready = 1;
}

static void mod_multiply(bn_t out, const bn_t a, const bn_t b) {
    bn_t product;
    mont_multiply(product, a, b);
    mont_multiply(out, product, r_squared);
}

static void mod_add(bn_t out, const bn_t a, const bn_t b) {
    uint64_t carry = 0;
    for (int i = 0; i < LIMBS; i++) {
        const uint64_t sum = (uint64_t)a[i] + b[i] + carry;
        out[i] = (uint32_t)sum;
        carry = sum >> 32;
    }
    if (carry || bn_compare(out, prime) >= 0) {
        bn_subtract(out, prime);
    }
}

static void mod_power(bn_t out, const bn_t base, const uint8_t *exponent, size_t length) {
    bn_t base_m, result;
    mont_multiply(base_m, base, r_squared);
    mont_multiply(result, bn_one, r_squared);
    for (size_t i = 0; i < length; i++) {
        for (int bit = 7; bit >= 0; bit--) {
            mont_multiply(result, result, result);
            if ((exponent[i] >> bit) & 1) {
                mont_multiply(result, result, base_m);
            }
        }
    }
    mont_multiply(out, result, bn_one);
}

static const uint8_t *strip_zeros(const uint8_t *bytes, size_t *length) {
    while (*length > 0 && bytes[0] == 0) {
        bytes++;
        (*length)--;
    }
    return bytes;
}

static void hash_padded_pair(const uint8_t *first, size_t first_length,
                             const uint8_t *second, size_t second_length,
                             uint8_t digest[20]) {
    uint8_t padded[2 * AIRPLAY_SRP_GROUP_SIZE];
    memset(padded, 0, sizeof(padded));
    memcpy(padded + AIRPLAY_SRP_GROUP_SIZE - first_length, first, first_length);
    memcpy(padded + 2 * AIRPLAY_SRP_GROUP_SIZE - second_length, second, second_length);
    airplay_sha1(padded, sizeof(padded), digest);
}

int airplay_srp_start(airplay_srp_t *session, const char *username, const char *pin,
                      const uint8_t *random) {
    if (!session || !username || !pin || !random ||
        strlen(username) >= sizeof(session->username)) {
        return -1;
    }
    group_init();
    memset(session, 0, sizeof(*session));
    memcpy(session->username, username, strlen(username) + 1);
    memcpy(session->salt, random, AIRPLAY_SRP_SALT_SIZE);
    if (session->salt[0] == 0) {
        session->salt[0] = 1;
    }
    memcpy(session->private_key, random + AIRPLAY_SRP_SALT_SIZE, AIRPLAY_SRP_PRIVATE_SIZE);

    /* x = H(salt | H(user ":" pin)), verifier = g^x */
    uint8_t inner[20], x[20];
    sha1_t sha;
    sha1_init(&sha);
    sha1_update(&sha, username, strlen(username));
    sha1_update(&sha, ":", 1);
    sha1_update(&sha, pin, strlen(pin));
    sha1_final(&sha, inner);
    sha1_init(&sha);
    sha1_update(&sha, session->salt, sizeof(session->salt));
    sha1_update(&sha, inner, sizeof(inner));
    sha1_final(&sha, x);

    bn_t generator, verifier;
    memset(generator, 0, sizeof(generator));
    generator[0] = 2;
    mod_power(verifier, generator, x, sizeof(x));
    bn_to_be(verifier, session->verifier);

    /* B = k * verifier + g^b, k = H(PAD(N) | PAD(g)) */
    const uint8_t generator_byte = 2;
    uint8_t k_hash[20];
    hash_padded_pair(prime_bytes, sizeof(prime_bytes), &generator_byte, 1, k_hash);
    bn_t k, kv, gb, public_key;
    bn_from_be(k, k_hash, sizeof(k_hash));
    mod_multiply(kv, k, verifier);
    mod_power(gb, generator, session->private_key, sizeof(session->private_key));
    mod_add(public_key, kv, gb);

    uint8_t padded[AIRPLAY_SRP_GROUP_SIZE];
    bn_to_be(public_key, padded);
    size_t length = sizeof(padded);
    const uint8_t *stripped = strip_zeros(padded, &length);
    memcpy(session->public_key, stripped, length);
    session->public_key_length = length;
    session->started = 1;
    return 0;
}

int airplay_srp_prove(airplay_srp_t *session, const uint8_t *client_public,
                      size_t client_public_length,
                      const uint8_t client_proof[AIRPLAY_SRP_PROOF_SIZE],
                      uint8_t server_proof[AIRPLAY_SRP_PROOF_SIZE]) {
    if (!session || !session->started || !client_public || !client_proof || !server_proof) {
        return -1;
    }
    group_init();
    size_t a_length = client_public_length;
    const uint8_t *a_bytes = strip_zeros(client_public, &a_length);
    if (a_length == 0 || a_length > AIRPLAY_SRP_GROUP_SIZE) {
        return -1;
    }
    bn_t a, verifier;
    bn_from_be(a, a_bytes, a_length);
    if (bn_compare(a, prime) >= 0) {
        bn_subtract(a, prime);
    }
    if (bn_is_zero(a)) {
        return -1;
    }
    bn_from_be(verifier, session->verifier, sizeof(session->verifier));

    /* S = (A * verifier^u)^b, u = H(PAD(A) | PAD(B)) */
    uint8_t u[20];
    hash_padded_pair(a_bytes, a_length, session->public_key, session->public_key_length, u);
    bn_t vu, base, secret;
    mod_power(vu, verifier, u, sizeof(u));
    mod_multiply(base, a, vu);
    mod_power(secret, base, session->private_key, sizeof(session->private_key));

    /* K = H(S | 00000000) | H(S | 00000001) */
    uint8_t secret_padded[AIRPLAY_SRP_GROUP_SIZE];
    bn_to_be(secret, secret_padded);
    size_t secret_length = sizeof(secret_padded);
    const uint8_t *secret_bytes = strip_zeros(secret_padded, &secret_length);
    for (uint8_t half = 0; half < 2; half++) {
        const uint8_t counter[4] = {0, 0, 0, half};
        sha1_t sha;
        sha1_init(&sha);
        sha1_update(&sha, secret_bytes, secret_length);
        sha1_update(&sha, counter, sizeof(counter));
        sha1_final(&sha, session->session_key + 20 * half);
    }

    /* M1 = H(H(N) xor H(g) | H(user) | salt | A | B | K), M2 = H(A | M1 | K) */
    const uint8_t generator_byte = 2;
    uint8_t hash_n[20], hash_g[20], hash_user[20], expected[20];
    airplay_sha1(prime_bytes, sizeof(prime_bytes), hash_n);
    airplay_sha1(&generator_byte, 1, hash_g);
    airplay_sha1((const uint8_t *)session->username, strlen(session->username), hash_user);
    for (int i = 0; i < 20; i++) {
        hash_n[i] ^= hash_g[i];
    }
    sha1_t sha;
    sha1_init(&sha);
    sha1_update(&sha, hash_n, sizeof(hash_n));
    sha1_update(&sha, hash_user, sizeof(hash_user));
    sha1_update(&sha, session->salt, sizeof(session->salt));
    sha1_update(&sha, a_bytes, a_length);
    sha1_update(&sha, session->public_key, session->public_key_length);
    sha1_update(&sha, session->session_key, sizeof(session->session_key));
    sha1_final(&sha, expected);

    uint8_t difference = 0;
    for (int i = 0; i < AIRPLAY_SRP_PROOF_SIZE; i++) {
        difference |= expected[i] ^ client_proof[i];
    }
    if (difference != 0) {
        memset(session->session_key, 0, sizeof(session->session_key));
        session->started = 0;
        return -1;
    }

    sha1_init(&sha);
    sha1_update(&sha, a_bytes, a_length);
    sha1_update(&sha, expected, sizeof(expected));
    sha1_update(&sha, session->session_key, sizeof(session->session_key));
    sha1_final(&sha, server_proof);
    session->proved = 1;
    return 0;
}

static void block_encrypt(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]) {
    struct AES_ctx ctx;
    AES_init_ctx(&ctx, key);
    memcpy(out, in, 16);
    AES_ECB_encrypt(&ctx, out);
}

static void gf_multiply(uint8_t x[16], const uint8_t h[16]) {
    uint8_t z[16] = {0};
    uint8_t v[16];
    memcpy(v, h, 16);
    for (int i = 0; i < 128; i++) {
        if ((x[i / 8] >> (7 - i % 8)) & 1) {
            for (int j = 0; j < 16; j++) {
                z[j] ^= v[j];
            }
        }
        const int low = v[15] & 1;
        for (int j = 15; j > 0; j--) {
            v[j] = (uint8_t)((v[j] >> 1) | (v[j - 1] << 7));
        }
        v[0] >>= 1;
        if (low) {
            v[0] ^= 0xe1;
        }
    }
    memcpy(x, z, 16);
}

static void ghash(uint8_t y[16], const uint8_t h[16], const uint8_t *data, size_t length) {
    while (length > 0) {
        const size_t take = length < 16 ? length : 16;
        for (size_t i = 0; i < take; i++) {
            y[i] ^= data[i];
        }
        gf_multiply(y, h);
        data += take;
        length -= take;
    }
}

static void ghash_lengths(uint8_t y[16], const uint8_t h[16], uint64_t first_bits,
                          uint64_t second_bits) {
    uint8_t block[16];
    for (int i = 0; i < 8; i++) {
        block[i] = (uint8_t)(first_bits >> (56 - 8 * i));
        block[8 + i] = (uint8_t)(second_bits >> (56 - 8 * i));
    }
    ghash(y, h, block, sizeof(block));
}

static void gcm_crypt(const uint8_t key[16], const uint8_t nonce[16], const uint8_t *in,
                      size_t length, uint8_t *out, const uint8_t *cipher, uint8_t tag[16]) {
    static const uint8_t zero[16] = {0};
    uint8_t h[16], j0[16] = {0}, counter[16], stream[16];
    block_encrypt(key, zero, h);
    /* A nonce that is not 96 bits long is hashed into the first counter block. */
    ghash(j0, h, nonce, 16);
    ghash_lengths(j0, h, 0, 128);

    uint8_t y[16] = {0};
    if (cipher) {
        ghash(y, h, cipher, length);
    }

    memcpy(counter, j0, 16);
    for (size_t offset = 0; offset < length; offset += 16) {
        for (int i = 15; i >= 12; i--) {
            if (++counter[i] != 0) {
                break;
            }
        }
        block_encrypt(key, counter, stream);
        const size_t take = length - offset < 16 ? length - offset : 16;
        for (size_t i = 0; i < take; i++) {
            out[offset + i] = in[offset + i] ^ stream[i];
        }
    }
    if (!cipher) {
        ghash(y, h, out, length);
    }
    ghash_lengths(y, h, 0, (uint64_t)length * 8);
    block_encrypt(key, j0, stream);
    for (int i = 0; i < 16; i++) {
        tag[i] = y[i] ^ stream[i];
    }
}

void airplay_gcm_encrypt(const uint8_t key[16], const uint8_t nonce[16],
                         const uint8_t *plain, size_t length, uint8_t *cipher,
                         uint8_t tag[16]) {
    gcm_crypt(key, nonce, plain, length, cipher, NULL, tag);
}

int airplay_gcm_decrypt(const uint8_t key[16], const uint8_t nonce[16],
                        const uint8_t *cipher, size_t length, const uint8_t tag[16],
                        uint8_t *plain) {
    uint8_t expected[16];
    gcm_crypt(key, nonce, cipher, length, plain, cipher, expected);
    uint8_t difference = 0;
    for (int i = 0; i < 16; i++) {
        difference |= expected[i] ^ tag[i];
    }
    if (difference != 0) {
        memset(plain, 0, length);
        return -1;
    }
    return 0;
}

static void derive(const char *label, const uint8_t session_key[AIRPLAY_SRP_KEY_SIZE],
                   uint8_t out[16]) {
    uint8_t input[64], digest[64];
    const size_t label_length = strlen(label);
    memcpy(input, label, label_length);
    memcpy(input + label_length, session_key, AIRPLAY_SRP_KEY_SIZE);
    crypto_hash(digest, input, label_length + AIRPLAY_SRP_KEY_SIZE);
    memcpy(out, digest, 16);
}

int airplay_srp_exchange_keys(airplay_srp_t *session, const uint8_t client_epk[32],
                              const uint8_t client_tag[16],
                              const uint8_t server_public_key[32],
                              uint8_t client_public_key[32], uint8_t server_epk[32],
                              uint8_t server_tag[16]) {
    if (!session || !session->proved) {
        return -1;
    }
    uint8_t key[16], nonce[16];
    derive("Pair-Setup-AES-Key", session->session_key, key);
    derive("Pair-Setup-AES-IV", session->session_key, nonce);
    nonce[15]++;
    const int result = airplay_gcm_decrypt(key, nonce, client_epk, 32, client_tag,
                                           client_public_key);
    if (result == 0) {
        nonce[15]++;
        airplay_gcm_encrypt(key, nonce, server_public_key, 32, server_epk, server_tag);
    }
    memset(session, 0, sizeof(*session));
    return result;
}
