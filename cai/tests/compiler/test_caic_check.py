import subprocess
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[3]
CAIC = REPO_ROOT / "cai" / "compiler" / "bin" / "caic"
GRAMMAR_PROGRAMS = REPO_ROOT / "cai" / "tests" / "grammar" / "programs"


class TestCaicCheck(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        subprocess.run(
            ["make", "-C", str(REPO_ROOT / "cai" / "compiler")],
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )

    def test_check_accepts_known_good_programs(self) -> None:
        programs = sorted(GRAMMAR_PROGRAMS.glob("*.ai"))
        self.assertGreater(len(programs), 0, "no sample programs found")

        for program in programs:
            r = subprocess.run(
                [str(CAIC), "check", str(program)],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )
            self.assertEqual(
                r.returncode,
                0,
                f"caic failed on {program.name}:\n{r.stderr}",
            )

    def test_check_rejects_syntax_errors(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            bad = Path(tmp) / "bad.ai"
            bad.write_text('fn main() -> int { let x = 1\\n', encoding="utf-8")
            r = subprocess.run(
                [str(CAIC), "check", str(bad)],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )
            self.assertNotEqual(r.returncode, 0)
            self.assertIn("error:", r.stderr)

    def test_check_rejects_top_level_let(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            bad = Path(tmp) / "bad.ai"
            bad.write_text("let x: int = 1\nfn main() -> int { return 0 }\n", encoding="utf-8")
            r = subprocess.run(
                [str(CAIC), "check", str(bad)],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )
            self.assertNotEqual(r.returncode, 0)
            self.assertIn("top-level variable declarations are not allowed", r.stderr)

    def test_check_rejects_yield_outside_async(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            bad = Path(tmp) / "bad.ai"
            bad.write_text("fn main() -> int { yield\nreturn 0 }\n", encoding="utf-8")
            r = subprocess.run(
                [str(CAIC), "check", str(bad)],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )
            self.assertNotEqual(r.returncode, 0)
            self.assertIn("`yield` is only allowed inside async functions", r.stderr)

    def test_check_rejects_await_outside_async(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            bad = Path(tmp) / "bad.ai"
            bad.write_text("fn main() -> int { let x = await f(); return 0 }\n", encoding="utf-8")
            r = subprocess.run(
                [str(CAIC), "check", str(bad)],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )
            self.assertNotEqual(r.returncode, 0)
            self.assertIn("`await` is only allowed inside async functions", r.stderr)
