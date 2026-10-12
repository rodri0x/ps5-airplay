"""Device check (run by hand, not by unittest): passcode pairing against the PS5 receiver.

    python3 tests/pin_device_check.py [host] [passcode]

Without a passcode it checks what can be checked without knowing it: the first
step answers with a salt and a public key, a wrong code is refused, and a
device that never entered the code cannot pair-verify.  With the passcode it
also completes the pairing and a pair-verify with the paired key.

The client side is independent of the C code: Python integers, hashlib,
plistlib and `cryptography`.
"""
import hashlib
import os
import plistlib
import socket
import sys

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey
from cryptography.hazmat.primitives.ciphers.aead import AESGCM

if len(sys.argv) < 2:
    raise SystemExit(f"usage: {sys.argv[0]} <ps5-host> [passcode]")
HOST = sys.argv[1]
PASSCODE = sys.argv[2] if len(sys.argv) > 2 else None
USER = "AA:BB:CC:DD:EE:01"
N = int(
    "AC6BDB41324A9A9BF166DE5E1389582FAF72B6651987EE07FC3192943DB56050A37329CBB4"
    "A099ED8193E0757767A13DD52312AB4B03310DCD7F48A9DA04FD50E8083969EDB767B0CF60"
    "95179A163AB3661A05FBD5FAAAE82918A9962F0B93B855F97993EC975EEAA80D740ADBF4FF"
    "747359D041D5C33EA71D281E446B14773BCA97B43A23FB801676BD207A436C6481F1D2B907"
    "8717461A5B9D32E688F87748544523B524B0D57D5EA77A2775D2ECFA032CFBDBF52FB37861"
    "60279004E57AE6AF874E7303CE53299CCC041C7BC308D82A5698F3A8D0C38271AE35F8E9DB"
    "FBB694B5C803D89F7AE435DE236D525F54759B65E372FCD68EF20FA7111F9E4AFF73", 16)
G = 2


def sha1(*parts):
    return hashlib.sha1(b"".join(parts)).digest()


def num(value):
    return value.to_bytes((value.bit_length() + 7) // 8, "big")


def pad(value):
    return value.to_bytes(256, "big")


class Rtsp:
    def __init__(self):
        self.sock = socket.create_connection((HOST, 7000), timeout=8)
        self.cseq = 1
        self.buf = b""

    def request(self, path, body=b"", plist=False):
        head = f"POST {path} RTSP/1.0\r\nCSeq: {self.cseq}\r\nUser-Agent: AirPlay/660.14\r\n"
        if body:
            kind = "application/x-apple-binary-plist" if plist else "application/octet-stream"
            head += f"Content-Type: {kind}\r\nContent-Length: {len(body)}\r\n"
        self.cseq += 1
        self.sock.sendall(head.encode() + b"\r\n" + body)
        while b"\r\n\r\n" not in self.buf:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise RuntimeError("connection closed before headers")
            self.buf += chunk
        header, _, rest = self.buf.partition(b"\r\n\r\n")
        length = 0
        for line in header.decode(errors="replace").split("\r\n")[1:]:
            if line.lower().startswith("content-length:"):
                length = int(line.split(":", 1)[1])
        while len(rest) < length:
            rest += self.sock.recv(4096)
        self.buf = rest[length:]
        return int(header.split(b" ")[1]), rest[:length]

    def plist(self, path, values):
        status, body = self.request(path, plistlib.dumps(values, fmt=plistlib.FMT_BINARY), True)
        return status, plistlib.loads(body) if body else {}


def pair(rtsp, code, identity):
    """Returns (status of the proof step, receiver's public key or None)."""
    status, _ = rtsp.request("/pair-pin-start")
    print(f"pair-pin-start -> {status}")
    status, first = rtsp.plist("/pair-setup-pin", {"method": "pin", "user": USER})
    if status != 200:
        print(f"pair-setup-pin step 1 -> {status} (is a passcode set on the console?)")
        return status, None
    salt, big_b = first["salt"], int.from_bytes(first["pk"], "big")
    print(f"step 1 -> 200, salt {len(salt)} bytes, pk {len(first['pk'])} bytes")
    a = int.from_bytes(os.urandom(32), "big")
    big_a = pow(G, a, N)
    k = int.from_bytes(sha1(pad(N), pad(G)), "big")
    u = int.from_bytes(sha1(pad(big_a), pad(big_b)), "big")
    x = int.from_bytes(sha1(salt, sha1(USER.encode(), b":", code.encode())), "big")
    secret = pow((big_b - k * pow(G, x, N)) % N, a + u * x, N)
    key = sha1(num(secret), b"\0\0\0\0") + sha1(num(secret), b"\0\0\0\1")
    h_xor = bytes(p ^ q for p, q in zip(sha1(num(N)), sha1(num(G))))
    m1 = sha1(h_xor, sha1(USER.encode()), salt, num(big_a), num(big_b), key)
    status, second = rtsp.plist("/pair-setup-pin", {"pk": num(big_a), "proof": m1})
    if status != 200:
        return status, None
    assert second["proof"] == sha1(num(big_a), m1, key), "receiver proof does not match"
    aes_key = hashlib.sha512(b"Pair-Setup-AES-Key" + key).digest()[:16]
    nonce = bytearray(hashlib.sha512(b"Pair-Setup-AES-IV" + key).digest()[:16])
    nonce[15] = (nonce[15] + 1) & 0xFF
    public = identity.public_key().public_bytes(serialization.Encoding.Raw,
                                                serialization.PublicFormat.Raw)
    sealed = AESGCM(aes_key).encrypt(bytes(nonce), public, None)
    status, third = rtsp.plist("/pair-setup-pin", {"epk": sealed[:32], "authTag": sealed[32:]})
    if status != 200:
        return status, None
    nonce[15] = (nonce[15] + 1) & 0xFF
    return 200, AESGCM(aes_key).decrypt(bytes(nonce), third["epk"] + third["authTag"], None)


def verify_step_one(rtsp, identity):
    ecdh = X25519PrivateKey.generate()
    raw = serialization.Encoding.Raw, serialization.PublicFormat.Raw
    body = b"\x01\x00\x00\x00" + ecdh.public_key().public_bytes(*raw) + \
        identity.public_key().public_bytes(*raw)
    return rtsp.request("/pair-verify", body)


def main():
    stranger = Ed25519PrivateKey.generate()
    status, body = verify_step_one(Rtsp(), stranger)
    print(f"pair-verify from an unpaired key -> {status}, {len(body)} bytes "
          f"({'refused' if not body else 'ACCEPTED: no passcode is being enforced'})")

    wrong = "0000" if PASSCODE != "0000" else "1111"
    status, _ = pair(Rtsp(), wrong, Ed25519PrivateKey.generate())
    print(f"wrong code {wrong} -> {status} ({'refused' if status == 470 else 'UNEXPECTED'})")

    if PASSCODE:
        identity = Ed25519PrivateKey.generate()
        status, receiver_key = pair(Rtsp(), PASSCODE, identity)
        print(f"correct code -> {status}, receiver key {receiver_key.hex() if receiver_key else None}")
        status, body = verify_step_one(Rtsp(), identity)
        print(f"pair-verify with the paired key -> {status}, {len(body)} bytes "
              f"({'accepted' if len(body) == 96 else 'REFUSED'})")


if __name__ == "__main__":
    main()
