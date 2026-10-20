import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from paths import ROOT

REPO = ROOT.parent
for folder in (REPO / "tools", REPO / "launcher"):
    if str(folder) not in sys.path:
        sys.path.insert(0, str(folder))

import apply_update  # noqa: E402
import updater  # noqa: E402


def make_item(base, name, text):
    (base / name).mkdir(parents=True)
    (base / name / "f.txt").write_text(text)


class RollbackTest(unittest.TestCase):
    def test_failed_restore_of_one_item_still_restores_the_other(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / "pkg"
            staged = Path(tmp) / "staged"
            for name in ("a", "b"):
                make_item(root, name, "old")
                make_item(staged, name, "new")
            real_retry = apply_update.retry
            real_copy = apply_update.copy_item

            def copy(source, target):
                if source.name == "b":
                    raise OSError("copy failed")
                real_copy(source, target)

            def remove(path):
                if path.name == "a":
                    raise OSError("locked")

            with mock.patch.object(apply_update, "copy_item", copy), \
                    mock.patch.object(apply_update, "remove_item", remove), \
                    mock.patch.object(apply_update, "retry", lambda action: real_retry(action, 0)):
                with self.assertRaises(OSError):
                    apply_update.apply(root, staged, lambda text: None)
            self.assertEqual((root / "b" / "f.txt").read_text(), "old")
            marker = root / "update" / "old" / "RESTORE-INCOMPLETE"
            self.assertEqual(marker.read_text().split(), ["a"])


class CleanupTest(unittest.TestCase):
    def test_cleanup_keeps_old_only_with_marker(self):
        with tempfile.TemporaryDirectory() as tmp:
            old = Path(tmp) / "update" / "old"
            old.mkdir(parents=True)
            (old / "RESTORE-INCOMPLETE").write_text("a\n")
            updater.cleanup(tmp)
            self.assertTrue(old.exists())
            (old / "RESTORE-INCOMPLETE").unlink()
            updater.cleanup(tmp)
            self.assertFalse(old.exists())


if __name__ == "__main__":
    unittest.main()
