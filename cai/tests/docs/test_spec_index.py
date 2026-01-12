import re
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[3]
CAI_DIR = REPO_ROOT / "cai"
SPEC_DIR = CAI_DIR / "docs" / "spec"
SPEC_INDEX = SPEC_DIR / "README.md"


class TestSpecIndex(unittest.TestCase):
    def test_spec_index_paths_exist(self) -> None:
        text = SPEC_INDEX.read_text(encoding="utf-8")
        referenced = re.findall(r"`(cai/docs/spec/[^`]+?\.md)`", text)
        self.assertGreater(len(referenced), 0, "spec index contains no referenced spec files")

        missing = []
        for rel in referenced:
            if not (REPO_ROOT / rel).is_file():
                missing.append(rel)

        self.assertEqual(missing, [], f"missing referenced spec files: {missing}")

    def test_all_spec_files_listed(self) -> None:
        text = SPEC_INDEX.read_text(encoding="utf-8")
        referenced = set(re.findall(r"`(cai/docs/spec/[^`]+?\.md)`", text))

        on_disk = {
            f"cai/docs/spec/{p.name}"
            for p in SPEC_DIR.glob("*.md")
            if p.name != "README.md"
        }

        self.assertEqual(
            referenced,
            on_disk,
            "spec index must list every spec file exactly once",
        )

    def test_no_legacy_spec_paths(self) -> None:
        needles = ["cai/" + "spec/"]
        exts = {".md", ".g4", ".py"}
        violations = []

        for path in list(CAI_DIR.rglob("*")) + [REPO_ROOT / "README.md"]:
            if not path.is_file():
                continue
            if path.suffix not in exts:
                continue
            content = path.read_text(encoding="utf-8", errors="ignore")
            for needle in needles:
                if needle in content:
                    violations.append(str(path.relative_to(REPO_ROOT)))
                    break

        self.assertEqual(violations, [], f"found legacy spec path references: {violations}")
