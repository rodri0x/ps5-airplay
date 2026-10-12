"""Checks the version comparison used by the update check (src/update_check.c)."""
import pathlib
import subprocess
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
HARNESS = ROOT / "build" / "version_host"
SOURCE = r"""
#include "update_check.h"
#include <stdio.h>
int main(int argc, char **argv) {
    if (argc != 3) return 2;
    int result = update_check_compare(argv[1], argv[2]);
    printf("%d\n", result < 0 ? -1 : result > 0 ? 1 : 0);
    return 0;
}
"""


class VersionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        (ROOT / "build").mkdir(exist_ok=True)
        main = ROOT / "build" / "version_host.c"
        main.write_text(SOURCE)
        subprocess.run(
            ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-DUPDATE_CHECK_HOST_BUILD",
             "-DAIRPLAY_VERSION=\"0.1\"", "-Isrc", "-Iinclude", "-o", str(HARNESS),
             str(main), "src/update_check.c"],
            cwd=ROOT, check=True)

    def compare(self, a, b):
        return int(subprocess.run([str(HARNESS), a, b], capture_output=True,
                                  text=True, check=True).stdout)

    def test_orders_versions_numerically(self):
        cases = [("0.1", "0.1", 0), ("v0.1", "0.1", 0), ("0.2", "0.1", 1),
                 ("0.10", "0.9", 1), ("1.0", "0.99", 1), ("0.1.1", "0.1", 1),
                 ("0.1", "0.1.0", 0), ("0.9", "v1.0", -1)]
        for a, b, expected in cases:
            self.assertEqual(self.compare(a, b), expected, (a, b))


if __name__ == "__main__":
    unittest.main()
