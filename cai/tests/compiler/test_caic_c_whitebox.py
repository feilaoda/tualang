import subprocess
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[3]


class TestCaicCWhitebox(unittest.TestCase):
    def test_make_test_runs(self) -> None:
        r = subprocess.run(
            ["make", "-C", str(REPO_ROOT / "cai" / "compiler"), "test"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        self.assertEqual(
            r.returncode,
            0,
            f"`make test` failed:\nstdout:\n{r.stdout}\nstderr:\n{r.stderr}",
        )

