"""Builds and runs tests/bplist_roundtrip.c against src/bplist.c."""
import pathlib
import subprocess
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class BplistTests(unittest.TestCase):
    def test_roundtrip(self):
        (ROOT / "build").mkdir(exist_ok=True)
        binary = ROOT / "build" / "bplist_roundtrip"
        subprocess.run(
            ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-Iinclude", "-Isrc",
             "-o", str(binary), "tests/bplist_roundtrip.c", "src/bplist.c"],
            cwd=ROOT, check=True)
        subprocess.run([str(binary)], cwd=ROOT, check=True)


if __name__ == "__main__":
    unittest.main()
