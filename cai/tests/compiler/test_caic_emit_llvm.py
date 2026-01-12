import subprocess
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[3]
CAIC = REPO_ROOT / "cai" / "compiler" / "bin" / "caic"
PROGRAM = REPO_ROOT / "cai" / "tests" / "grammar" / "programs" / "01_basics.ai"


class TestCaicEmitLlvm(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        subprocess.run(
            ["make", "-C", str(REPO_ROOT / "cai" / "compiler")],
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )

    def test_emit_llvm_outputs_prototypes(self) -> None:
        r = subprocess.run(
            [str(CAIC), "emit-llvm", str(PROGRAM)],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("declare i32 @add(i32, i32)", r.stdout)
        self.assertIn("declare i32 @main()", r.stdout)

