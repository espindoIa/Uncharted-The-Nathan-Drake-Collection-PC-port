import sys
import tempfile
import unittest
from pathlib import Path

from paths import ROOT

TOOLS = str(ROOT.parent / "tools")
if TOOLS not in sys.path:
    sys.path.insert(0, TOOLS)

from probe_uncharted import backup_saves  # noqa: E402


def make_user(tmp):
    user = Path(tmp)
    save = user / "savedata/1/CUSA02320/slot"
    save.mkdir(parents=True)
    (save / "data.bin").write_bytes(b"abc")
    return user


def backups(user):
    folder = user / "save-backups"
    return sorted(folder.iterdir()) if folder.is_dir() else []


class SaveBackupTest(unittest.TestCase):
    def test_creates_a_backup_with_the_files(self):
        with tempfile.TemporaryDirectory() as tmp:
            user = make_user(tmp)
            backup_saves(user)
            made = backups(user)
            self.assertEqual(len(made), 1)
            self.assertEqual((made[0] / "1/CUSA02320/slot/data.bin").read_bytes(), b"abc")

    def test_keeps_only_the_newest_five(self):
        with tempfile.TemporaryDirectory() as tmp:
            user = make_user(tmp)
            for i in range(7):
                (user / "save-backups" / f"2026010{i}-000000").mkdir(parents=True)
            (user / "savedata/1/CUSA02320/slot/data.bin").write_bytes(b"changed!")
            backup_saves(user)
            names = [p.name for p in backups(user)]
            self.assertEqual(len(names), 5)
            self.assertNotIn("20260100-000000", names)
            self.assertNotIn("20260101-000000", names)
            self.assertNotIn("20260102-000000", names)

    def test_skips_when_unchanged(self):
        with tempfile.TemporaryDirectory() as tmp:
            user = make_user(tmp)
            backup_saves(user)
            first = backups(user)
            backup_saves(user)
            self.assertEqual(backups(user), first)

    def test_skips_when_savedata_is_missing_or_empty(self):
        with tempfile.TemporaryDirectory() as tmp:
            backup_saves(Path(tmp))
            self.assertEqual(backups(Path(tmp)), [])
            (Path(tmp) / "savedata/1").mkdir(parents=True)
            backup_saves(Path(tmp))
            self.assertEqual(backups(Path(tmp)), [])


if __name__ == "__main__":
    unittest.main()
