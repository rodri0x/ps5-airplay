"""Checks src/airplay_srp.c against an independent client.

The client side below is written from the protocol description (SRP-6a in
Apple's variant, as UxPlay documents it) with Python integers, hashlib and the
`cryptography` AES-GCM.  The C code under test never shares code with it.
"""
import hashlib
import os
import pathlib
import subprocess
import unittest

from cryptography.hazmat.primitives.ciphers.aead import AESGCM

ROOT = pathlib.Path(__file__).resolve().parents[1]
HARNESS = ROOT / "build" / "srp_host"

N_HEX = (
    "AC6BDB41324A9A9BF166DE5E1389582FAF72B6651987EE07FC3192943DB56050A37329CBB4"
    "A099ED8193E0757767A13DD52312AB4B03310DCD7F48A9DA04FD50E8083969EDB767B0CF60"
    "95179A163AB3661A05FBD5FAAAE82918A9962F0B93B855F97993EC975EEAA80D740ADBF4FF"
    "747359D041D5C33EA71D281E446B14773BCA97B43A23FB801676BD207A436C6481F1D2B907"
    "8717461A5B9D32E688F87748544523B524B0D57D5EA77A2775D2ECFA032CFBDBF52FB37861"
    "60279004E57AE6AF874E7303CE53299CCC041C7BC308D82A5698F3A8D0C38271AE35F8E9DB"
    "FBB694B5C803D89F7AE435DE236D525F54759B65E372FCD68EF20FA7111F9E4AFF73"
)
N = int(N_HEX, 16)
G = 2


def sha1(*parts: bytes) -> bytes:
    return hashlib.sha1(b"".join(parts)).digest()


def num(value: int) -> bytes:
    """Big-endian bytes without leading zeros, like BN_bn2bin."""
    return value.to_bytes((value.bit_length() + 7) // 8, "big")


def pad(value: int) -> bytes:
    return value.to_bytes(256, "big")


class Harness:
    def __init__(self):
        self.process = subprocess.Popen([str(HARNESS)], stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, text=True)

    def ask(self, *words: str) -> list[str]:
        self.process.stdin.write(" ".join(words) + "\n")
        self.process.stdin.flush()
        return self.process.stdout.readline().split()

    def close(self):
        self.process.stdin.close()
        self.process.stdout.close()
        self.process.wait()


class SrpTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        build = ROOT / "build"
        build.mkdir(exist_ok=True)
        include = ["-Isrc", "-Ithird_party/tweetnacl"]
        # Third-party sources are built as shipped; the project's own with -Werror.
        third_party = {"aes": "src/crypto/aes.c", "tweetnacl": "third_party/tweetnacl/tweetnacl.c"}
        objects = []
        for name, source in third_party.items():
            target = str(build / f"srp_host_{name}.o")
            subprocess.run(["cc", "-std=c11", "-O1", "-w", *include, "-c", "-o", target, source],
                           cwd=ROOT, check=True)
            objects.append(target)
        subprocess.run(
            ["cc", "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror", *include,
             "-o", str(HARNESS), "tests/srp_host.c", "src/airplay_srp.c",
             "tests/host_randombytes.c", *objects],
            cwd=ROOT, check=True)

    def setUp(self):
        self.harness = Harness()
        self.addCleanup(self.harness.close)

    def test_sha1_matches_hashlib(self):
        for size in (0, 1, 55, 56, 63, 64, 65, 200, 1000):
            data = os.urandom(size)
            reply = self.harness.ask("sha1", *([data.hex()] if size else []))
            self.assertEqual(reply[0], hashlib.sha1(data).hexdigest(), size)

    def test_gcm_matches_cryptography(self):
        for size in (1, 15, 16, 17, 32, 100):
            key, nonce, plain = os.urandom(16), os.urandom(16), os.urandom(size)
            cipher, tag, good, bad = self.harness.ask("gcm", key.hex(), nonce.hex(), plain.hex())
            expected = AESGCM(key).encrypt(nonce, plain, None)
            self.assertEqual(cipher + tag, expected.hex(), size)
            self.assertEqual((good, bad), ("0", "-1"))

    def pair(self, server_pin: str, client_pin: str, salt_first_byte=None):
        user = "AA:BB:CC:DD:EE:FF"
        random = bytearray(os.urandom(48))
        if salt_first_byte is not None:
            random[0] = salt_first_byte
        salt_hex, b_hex = self.harness.ask("start", user, server_pin, bytes(random).hex())
        salt, big_b = bytes.fromhex(salt_hex), int(b_hex, 16)

        a = int.from_bytes(os.urandom(32), "big")
        big_a = pow(G, a, N)
        k = int.from_bytes(sha1(pad(N), pad(G)), "big")
        u = int.from_bytes(sha1(pad(big_a), pad(big_b)), "big")
        x = int.from_bytes(sha1(salt, sha1(user.encode(), b":", client_pin.encode())), "big")
        secret = pow((big_b - k * pow(G, x, N)) % N, a + u * x, N)
        key = sha1(num(secret), b"\0\0\0\0") + sha1(num(secret), b"\0\0\0\1")
        h_xor = bytes(p ^ q for p, q in zip(sha1(num(N)), sha1(num(G))))
        m1 = sha1(h_xor, sha1(user.encode()), salt, num(big_a), num(big_b), key)
        reply = self.harness.ask("prove", num(big_a).hex(), m1.hex())
        return reply, big_a, m1, key

    def test_correct_pin_pairs_and_exchanges_keys(self):
        for first in (None, 0):  # 0: a salt that would start with a zero byte
            reply, big_a, m1, key = self.pair("4821", "4821", first)
            self.assertEqual(reply[0], "ok")
            self.assertEqual(reply[1], sha1(num(big_a), m1, key).hex())
            self.assertEqual(reply[2], key.hex())

            aes_key = hashlib.sha512(b"Pair-Setup-AES-Key" + key).digest()[:16]
            nonce = bytearray(hashlib.sha512(b"Pair-Setup-AES-IV" + key).digest()[:16])
            client_pk, server_pk = os.urandom(32), os.urandom(32)
            nonce[15] = (nonce[15] + 1) & 0xFF
            sealed = AESGCM(aes_key).encrypt(bytes(nonce), client_pk, None)
            reply = self.harness.ask("keys", sealed[:32].hex(), sealed[32:].hex(), server_pk.hex())
            self.assertEqual(reply[0], "ok")
            self.assertEqual(reply[1], client_pk.hex())
            nonce[15] = (nonce[15] + 1) & 0xFF
            opened = AESGCM(aes_key).decrypt(bytes(nonce), bytes.fromhex(reply[2] + reply[3]), None)
            self.assertEqual(opened, server_pk)

    def test_wrong_pin_is_refused(self):
        reply, _, _, _ = self.pair("4821", "4822")
        self.assertEqual(reply, ["fail"])

    def test_key_exchange_needs_a_proved_session(self):
        reply = self.harness.ask("keys", "00" * 32, "00" * 16, "00" * 32)
        self.assertEqual(reply, ["fail"])


if __name__ == "__main__":
    unittest.main()
