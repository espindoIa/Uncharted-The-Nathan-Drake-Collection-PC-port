"""Headless smoke test of launcher/uncharted_launcher.pyw (a real Tk root, withdrawn).

Run: py -m unittest test_uncharted_launcher_ui  (from gow3_pc/tests; needs Tk and Pillow).
"""
import importlib.machinery
import importlib.util
import sys
import tempfile
import time
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

from paths import ROOT

REPO = ROOT.parent
LAUNCHER = REPO / "launcher"
if str(LAUNCHER) not in sys.path:
    sys.path.insert(0, str(LAUNCHER))

try:
    import tkinter as tk
    import PIL  # noqa: F401
    loader = importlib.machinery.SourceFileLoader("uncharted_launcher", str(LAUNCHER / "uncharted_launcher.pyw"))
    spec = importlib.util.spec_from_loader("uncharted_launcher", loader)
    uncharted_launcher = importlib.util.module_from_spec(spec)
    loader.exec_module(uncharted_launcher)
    IMPORT_ERROR = None
except ImportError as error:  # no tkinter or Pillow here
    uncharted_launcher = None
    IMPORT_ERROR = error


@unittest.skipIf(uncharted_launcher is None, f"launcher needs tkinter and Pillow: {IMPORT_ERROR}")
class LauncherUiTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        user = Path(self.tmp.name) / "user"
        mod = uncharted_launcher
        # The real %APPDATA% config and the repo's analysis/ folder must not be read or written.
        for name, value in (("USER_DIR", user), ("INI_FILE", user / "probe.ini"),
                            ("CONFIG_FILE", Path(self.tmp.name) / "launcher.json"),
                            ("PREFIX_CONFIG_FILE", Path(self.tmp.name) / "launcher.json"),
                            ("_VULKAN_GPUS", []), ("gpu_names", lambda: [])):
            patcher = mock.patch.object(mod, name, value)
            patcher.start()
            self.addCleanup(patcher.stop)
        try:
            self.root = tk.Tk()
        except tk.TclError as error:
            self.skipTest(f"no Tk display: {error}")
        self.addCleanup(self.root.destroy)
        # Pending after() timers of the launcher would fire on a destroyed root (noise on stderr).
        self.addCleanup(lambda: [self.root.after_cancel(i) for i in self.root.tk.splitlist(self.root.tk.call("after", "info"))])
        self.root.withdraw()
        self.launcher = mod.Launcher(self.root)

    def pump(self, until, seconds=5):
        end = time.monotonic() + seconds
        while not until() and time.monotonic() < end:
            self.root.update()
            time.sleep(0.01)
        return until()

    def test_builds_and_every_section_shows(self):
        self.assertEqual(set(self.launcher.pages),
                         {"home", "display", "performance", "controls", "gameplay", "system"})
        for name in self.launcher.pages:
            self.launcher.show_tab(name, save=False)
            self.root.update()
            self.assertEqual(self.launcher.section, name)

    def test_old_tab_names_map_to_sections(self):
        mod = uncharted_launcher
        for old, new in (("setup", "home"), ("cheats", "gameplay"), ("advanced", "system"),
                         ("display", "display"), ("nonsense", "home"), (None, "home")):
            self.assertEqual(mod.section_name(old), new)
        mod.CONFIG_FILE.write_text('{"version": 8, "tab": "advanced"}', encoding="utf-8")
        self.assertEqual(mod.read_config()["tab"], "system")
        self.launcher.show_tab("setup")
        self.assertEqual(self.launcher.section, "home")
        self.assertEqual(self.launcher.config["tab"], "home")

    def test_change_reaches_the_games_ini(self):
        game = self.launcher.config["game"]
        self.launcher.change("vsync", "0")
        self.assertEqual(uncharted_launcher.read_ini(game)["vsync"], "0")
        self.assertTrue(uncharted_launcher.ini_file(game).is_file())

    def test_wheel_binding_survives_a_capture(self):
        launcher, root = self.launcher, self.root
        self.assertIn("on_wheel", root.bind_all("<MouseWheel>"))
        launcher.start_capture("cross", 0)
        root.update()
        # The slot's own click must not count: nothing is bound before the arming delay.
        self.assertIsNone(launcher.on_key(SimpleNamespace(keysym="X")))
        launcher.arm_capture()
        launcher.stop_capture()
        self.assertIn("on_wheel", root.bind_all("<MouseWheel>"))
        self.assertTrue(root.bind_all("<KeyPress>") and root.bind_all("<ButtonPress>"))
        launcher.start_capture("cross", 0)
        launcher.arm_capture()
        self.assertEqual(launcher.on_key(SimpleNamespace(keysym="x")), "break")  # captured through the router
        self.assertEqual(launcher.game_controls()["cross"], ["key:X"])
        self.assertIn("on_wheel", root.bind_all("<MouseWheel>"))
        self.assertIsNone(launcher.on_key(SimpleNamespace(keysym="y")))  # capture over: normal handling
        launcher.set_binding("cross", 0, "key:Z")
        self.assertIn("on_wheel", root.bind_all("<MouseWheel>"))

    def test_reset_puts_the_default_controls_in_the_ini(self):
        launcher = self.launcher
        game = launcher.config["game"]
        launcher.set_binding("cross", 0, "key:X")
        launcher.set_pad_binding("cross", "b")
        values = uncharted_launcher.read_ini(game)
        self.assertIn("cross=X", values["key_map"])
        with mock.patch.object(uncharted_launcher.messagebox, "askyesno", return_value=True):
            launcher.reset()
        settings = {}
        self.assertTrue(uncharted_launcher.controls_from_ini(uncharted_launcher.read_ini(game), settings))
        self.assertEqual(settings["controls"]["cross"], ["key:Space"])
        self.assertEqual(settings["pad_controls"], {})
        self.assertIsNone(launcher.game_settings()["controls"])

    def test_stop_marks_stopping_and_runs_taskkill_in_a_thread(self):
        launcher = self.launcher
        called = []
        launcher.process = SimpleNamespace(pid=4242, poll=lambda: None)
        self.addCleanup(setattr, launcher, "process", None)
        with mock.patch.object(uncharted_launcher.subprocess, "run", lambda args, **kw: called.append(args)):
            launcher.stop()
            self.assertTrue(launcher.stopping)
            self.assertTrue(self.pump(lambda: bool(called)))
        self.assertEqual(called[0][:2], ["taskkill", "/PID"])
        self.assertIn("4242", called[0])


if __name__ == "__main__":
    unittest.main()
