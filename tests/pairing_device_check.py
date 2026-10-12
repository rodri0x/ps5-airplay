"""Device check (run by hand, not by unittest): client-side AirPlay pair-setup / pair-verify check against the PS5 receiver.

Uses the standard `cryptography` primitives, independent of the C code under
test.  Message layout follows UxPlay/RPiPlay (pair-setup carries 32-byte
Ed25519 keys; pair-verify step 1 is 0x01 000000 + X25519 + Ed25519, step 2 is
0x00 000000 + AES-CTR-encrypted signature)."""

import hashlib
import socket
import sys

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey, Ed25519PublicKey
from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey, X25519PublicKey
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

if len(sys.argv) < 2:
    raise SystemExit(f"usage: {sys.argv[0]} <ps5-host>")
HOST = sys.argv[1]
PORT = 7000


def raw_public(key):
    return key.public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)


class Rtsp:
    def __init__(self):
        self.sock = socket.create_connection((HOST, PORT), timeout=6)
        self.cseq = 1
        self.buf = b""

    def close(self):
        self.sock.close()

    def request(self, method, path, body=b""):
        head = f"{method} {path} RTSP/1.0\r\nCSeq: {self.cseq}\r\nUser-Agent: AirPlay/660.14\r\n"
        if body:
            head += "Content-Type: application/octet-stream\r\n"
            head += f"Content-Length: {len(body)}\r\n"
        head += "\r\n"
        self.cseq += 1
        self.sock.sendall(head.encode() + body)
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
            chunk = self.sock.recv(4096)
            if not chunk:
                raise RuntimeError("connection closed mid-body")
            rest += chunk
        self.buf = rest[length:]
        status = int(header.split(b" ")[1])
        return status, rest[:length]


def ctr_stream(shared):
    key = hashlib.sha512(b"Pair-Verify-AES-Key" + shared).digest()[:16]
    iv = hashlib.sha512(b"Pair-Verify-AES-IV" + shared).digest()[:16]
    return Cipher(algorithms.AES(key), modes.CTR(iv)).encryptor()


def run(tamper=False):
    client_ed = Ed25519PrivateKey.generate()
    client_ed_pk = raw_public(client_ed.public_key())
    rtsp = Rtsp()
    results = {}

    status, body = rtsp.request("OPTIONS", "*")
    results["OPTIONS"] = status

    status, server_ed_pk = rtsp.request("POST", "/pair-setup", client_ed_pk)
    results["pair-setup"] = (status, len(server_ed_pk), server_ed_pk.hex())
    if status != 200 or len(server_ed_pk) != 32:
        rtsp.close()
        return results

    client_x = X25519PrivateKey.generate()
    client_x_pk = raw_public(client_x.public_key())
    step1 = bytes([1, 0, 0, 0]) + client_x_pk + client_ed_pk
    status, resp = rtsp.request("POST", "/pair-verify", step1)
    results["pair-verify step1"] = (status, len(resp))
    if status != 200 or len(resp) != 96:
        rtsp.close()
        return results

    server_x_pk, enc_sig = resp[:32], resp[32:]
    shared = client_x.exchange(X25519PublicKey.from_public_bytes(server_x_pk))
    stream = ctr_stream(shared)
    server_sig = stream.update(enc_sig)  # keystream bytes 0..63
    try:
        Ed25519PublicKey.from_public_bytes(server_ed_pk).verify(
            server_sig, server_x_pk + client_x_pk)
        results["receiver signature"] = "verified"
    except Exception as error:  # noqa: BLE001 - report any failure
        results["receiver signature"] = f"FAILED: {error!r}"

    client_sig = client_ed.sign(client_x_pk + server_x_pk)
    enc_client_sig = stream.update(client_sig)  # continues at keystream byte 64
    if tamper:
        enc_client_sig = bytes([enc_client_sig[0] ^ 1]) + enc_client_sig[1:]
    step2 = bytes([0, 0, 0, 0]) + enc_client_sig
    status, _ = rtsp.request("POST", "/pair-verify", step2)
    results["pair-verify step2" + (" (tampered)" if tamper else "")] = status

    if not tamper and status == 200:
        status, info = rtsp.request("GET", "/info")
        results["GET /info after verify"] = status
    rtsp.close()
    return results


if __name__ == "__main__":
    print("== normal pairing")
    for key, value in run().items():
        print(f"  {key}: {value}")
    print("== tampered client signature")
    for key, value in run(tamper=True).items():
        print(f"  {key}: {value}")
