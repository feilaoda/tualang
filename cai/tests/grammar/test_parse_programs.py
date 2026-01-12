import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[3]
GRAMMAR = REPO_ROOT / "cai" / "parser.g4"
PROGRAMS_DIR = REPO_ROOT / "cai" / "tests" / "grammar" / "programs"
BAD_PROGRAMS_DIR = REPO_ROOT / "cai" / "tests" / "grammar" / "bad"

def _have_antlr() -> bool:
    if os.environ.get("CAI_SKIP_ANTLR") == "1":
        return False
    return (
        shutil.which("antlr") is not None
        and shutil.which("javac") is not None
        and shutil.which("grun") is not None
    )


class TestGrammarPrograms(unittest.TestCase):
    @unittest.skipUnless(_have_antlr(), "antlr not available (or CAI_SKIP_ANTLR=1)")
    def test_programs_parse_without_syntax_errors(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            src_dir = Path(tmp) / "src"
            classes_dir = Path(tmp) / "classes"
            src_dir.mkdir(parents=True, exist_ok=True)
            classes_dir.mkdir(parents=True, exist_ok=True)

            # ANTLR requires the `.g4` filename to match the `grammar <Name>;`
            # declaration; keep `cai/parser.g4` as the canonical file in-repo.
            grammar_copy = Path(tmp) / "caiparser.g4"
            grammar_copy.write_text(GRAMMAR.read_text(encoding="utf-8"), encoding="utf-8")

            gen_cmd = [
                "antlr",
                "-Dlanguage=Java",
                "-visitor",
                "-no-listener",
                "-o",
                str(src_dir),
                str(grammar_copy),
            ]
            subprocess.run(gen_cmd, check=True, cwd=REPO_ROOT, stdout=subprocess.PIPE, stderr=subprocess.PIPE)

            antlr_script = shutil.which("antlr")
            if antlr_script is None:  # pragma: no cover
                self.fail("antlr not found")
            antlr_sh = Path(antlr_script).read_text(encoding="utf-8", errors="ignore")
            jar_marker = 'CLASSPATH="'
            jar_start = antlr_sh.find(jar_marker)
            if jar_start == -1:  # pragma: no cover
                self.fail("unable to locate ANTLR jar path from the `antlr` wrapper script")
            jar_start += len(jar_marker)
            jar_end = antlr_sh.find(":", jar_start)
            if jar_end == -1:  # pragma: no cover
                self.fail("unable to parse ANTLR jar path from the `antlr` wrapper script")
            antlr_jar = antlr_sh[jar_start:jar_end]

            java_files = sorted(str(p) for p in src_dir.glob("*.java"))
            self.assertGreater(len(java_files), 0, "ANTLR Java generation produced no .java files")

            javac_cmd = [
                "javac",
                "-cp",
                antlr_jar,
                "-d",
                str(classes_dir),
                *java_files,
            ]
            subprocess.run(javac_cmd, check=True, cwd=REPO_ROOT, stdout=subprocess.PIPE, stderr=subprocess.PIPE)

            for program in sorted(PROGRAMS_DIR.glob("*.ai")):
                rig_cmd = ["grun", "caiparser", "compilationUnit", str(program)]
                result = subprocess.run(
                    rig_cmd,
                    cwd=classes_dir,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    text=True,
                )

                error_lines = [
                    line for line in result.stderr.splitlines() if line.strip().startswith("line ")
                ]
                self.assertEqual(
                    error_lines,
                    [],
                    f"{program.name} has syntax errors:\n" + "\n".join(error_lines),
                )

            for program in sorted(BAD_PROGRAMS_DIR.glob("*.ai")):
                rig_cmd = ["grun", "caiparser", "compilationUnit", str(program)]
                result = subprocess.run(
                    rig_cmd,
                    cwd=classes_dir,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    text=True,
                )

                error_lines = [
                    line for line in result.stderr.splitlines() if line.strip().startswith("line ")
                ]
                self.assertNotEqual(
                    error_lines,
                    [],
                    f"{program.name} should have syntax errors but parsed cleanly",
                )
