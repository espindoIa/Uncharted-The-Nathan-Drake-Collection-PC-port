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


def ready(**changes):
    """readiness() with everything fine, except `changes`."""
    facts = dict(game_ok=True, game_version="01.02", game_dir="D:/game", update_found=True, update_version="01.02",
                 update_problems=[], bridge_present=False, dlss_present=False, fg_present=False, slow_drive=None,
                 release_version=None)
    facts.update(changes)
    return uncharted_launcher.readiness(**facts)


@unittest.skipIf(uncharted_launcher is None, f"launcher needs tkinter and Pillow: {IMPORT_ERROR}")
class ReadinessTest(unittest.TestCase):
    def test_no_game_folder_is_numbered_first_run_steps(self):
        items = ready(game_ok=False, game_version=None, update_found=False, update_version=None, bridge_present=True)
        self.assertEqual([i["key"] for i in items], ["game", "update", "dlss"])
        self.assertEqual(items[0]["state"], "missing")
        self.assertEqual([i["title"][:2] for i in items], ["1.", "2.", "3."])
        self.assertEqual([a[1] for a in items[0]["actions"]], ["choose_game_dir", "install_game_pkg"])
        self.assertNotEqual(items[1]["state"], "ok")

    def test_game_without_update_is_not_ready(self):
        items = ready(game_version="01.00", update_found=False, update_version=None)
        self.assertEqual([i["state"] for i in items], ["ok", "warn"])
        self.assertEqual([a[1] for a in items[1]["actions"]], ["choose_update_folder", "install_game_update"])
        self.assertFalse(items[1]["title"][0].isdigit())  # numbered only without a game folder
        # 1.02 merged into the game folder, or a damaged update folder:
        self.assertEqual(ready(game_version="01.02", update_found=False)[1]["state"], "ok")
        self.assertEqual(ready(update_problems=["u3data/build/main/bin.psarc (damaged)"])[1]["state"], "warn")

    def test_everything_present_is_all_ok(self):
        items = ready()
        self.assertEqual([i["key"] for i in items], ["game", "update"])
        self.assertTrue(all(i["state"] == "ok" for i in items))
        items = ready(bridge_present=True, dlss_present=True, fg_present=True)
        self.assertEqual([i["key"] for i in items], ["game", "update", "dlss"])
        self.assertTrue(all(i["state"] == "ok" for i in items))

    def test_dll_rows_follow_the_bridge_and_the_dlls(self):
        items = ready(bridge_present=True)
        self.assertEqual((items[2]["key"], items[2]["state"]), ("dlss", "optional"))
        self.assertEqual([a[1] for a in items[2]["actions"]], ["get_nvidia", "add_dlss"])
        items = ready(bridge_present=True, dlss_present=True)
        self.assertEqual([i["key"] for i in items[2:]], ["dlss", "framegen"])
        self.assertEqual([a[1] for a in items[3]["actions"]], ["get_nvidia", "add_framegen"])

    def test_slow_drive_and_launcher_release_add_warnings(self):
        items = ready(slow_drive="E: is a USB drive", release_version="0.6.0")
        self.assertEqual([(i["key"], i["state"]) for i in items[2:]], [("drive", "warn"), ("launcher", "warn")])
        self.assertIn("E: is a USB drive", items[2]["detail"])
        self.assertIn("SSD", items[2]["detail"])
        self.assertIn("0.6.0", items[3]["title"])
        self.assertEqual(items[3]["actions"], [("Install", "install_update")])


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

    def test_refresh_home_with_a_folder_that_is_not_a_game(self):
        launcher, mod = self.launcher, uncharted_launcher
        fake = Path(self.tmp.name) / "game"
        fake.mkdir()
        (fake / "eboot.bin").write_bytes(b"x")  # no sce_sys/param.sfo: not a game folder
        launcher.config["game_dir"] = str(fake)
        self.assertFalse(mod.is_game_dir(fake))
        launcher.refresh_home()
        self.assertTrue(self.pump(lambda: launcher.home_checks is not None and not launcher.home_busy))
        self.assertEqual(launcher.home_shown[0]["state"], "missing")
        self.assertIn("1.", launcher.home_shown[0]["title"])
        self.assertTrue(launcher.home_list.winfo_children())

    def test_rebuild_sections_keeps_widgets_and_the_shown_section(self):
        launcher, mod = self.launcher, uncharted_launcher
        launcher.show_tab("display", save=False)
        old_page = launcher.pages["display"]
        with mock.patch.object(mod, "anti_aliasing_options", lambda: [("off", "Off"), ("dlss", "DLSS")]):
            launcher.rebuild_sections()
        self.root.update()
        self.assertIsNot(launcher.pages["display"], old_page)
        self.assertIs(launcher.display_page, launcher.pages["display"])
        self.assertFalse(old_page.winfo_exists())
        self.assertTrue(launcher.widgets["upscaler"].winfo_exists())
        self.assertIn("dlss", launcher.widgets["upscaler"].labels)
        self.assertTrue(launcher.widgets["fps_limit"].winfo_exists())
        self.assertEqual(launcher.section, "display")
        self.assertEqual(launcher.pages["display"].holder.winfo_manager(), "pack")  # packed = shown (root is withdrawn)
        self.assertEqual(launcher.pages["home"].holder.winfo_manager(), "")

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
        launcher.ask_yesno = lambda *args, **kwargs: True
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

    def test_crash_report_and_dialog_buttons(self):
        launcher = self.launcher
        reason = "Fault: access violation\nhost #0 foo\n\nPlease attach boot.log."
        report = launcher.crash_report(reason)
        for part in (reason, launcher.current_game()["name"], "upscaler:", "fps_limit:", "Graphics card:", "Log:"):
            self.assertIn(part, report)
        seen = {}

        def fake_ask(root, title, message, buttons, **kwargs):
            seen.update(title=title, message=message, labels=[b[0] for b in buttons], detail=kwargs["detail"])
            return "report"
        with mock.patch.object(uncharted_launcher.Dialog, "ask", fake_ask), \
                mock.patch.object(launcher, "toast") as toast:
            launcher.show_crash(reason)
        self.assertEqual(seen["title"], "The game stopped")
        self.assertEqual(seen["message"], "Fault: access violation")
        self.assertEqual(seen["labels"], ["Open log folder", "Copy report", "Close"])
        self.assertEqual(seen["detail"], "host #0 foo\n\nPlease attach boot.log.")
        self.assertEqual(self.root.clipboard_get(), report)
        toast.assert_called_once_with("Report copied", "ok")

    def test_every_long_hint_has_a_short_line_and_global_rows_a_badge(self):
        mod, launcher = uncharted_launcher, self.launcher
        launcher.add_gpu_row([("0", "GPU A"), ("1", "GPU B")])  # the row that joins Display later
        self.assertGreater(len(launcher.rows), 20)
        glob = {"Language", "Speakers", "Unlock all chapters", "Unlock all rewards", "Performance log",
                "Torch follows Nate (Uncharted 2)", "Check for updates at start", "Idle game threads",
                *(title for _, title, _ in mod.CHEATS)}
        for row in launcher.rows:
            title = row["title"]
            if len(row["hint"]) > mod.HINT_MAX:
                self.assertTrue(row["short"], f"{title}: hint of {len(row['hint'])} characters has no short line")
            if row["short"]:
                self.assertLessEqual(len(row["short"]), mod.SHORT_MAX, title)
                self.assertLess(len(row["short"]), len(row["hint"]), title)
            self.assertEqual(row["badge"], "ALL GAMES" if title in glob else None, title)
        self.assertTrue(glob <= {row["title"] for row in launcher.rows}, "a global row was not built")

    def test_launcher_uses_no_native_message_box(self):
        text = (LAUNCHER / "uncharted_launcher.pyw").read_text(encoding="utf-8")
        self.assertNotIn("messagebox.", text.split('if __name__ == "__main__"')[0])


@unittest.skipIf(uncharted_launcher is None, f"launcher needs tkinter and Pillow: {IMPORT_ERROR}")
class DialogTest(unittest.TestCase):
    def setUp(self):
        try:
            self.root = tk.Tk()
        except tk.TclError as error:
            self.skipTest(f"no Tk display: {error}")
        self.addCleanup(self.root.destroy)
        self.addCleanup(lambda: [self.root.after_cancel(i) for i in self.root.tk.splitlist(self.root.tk.call("after", "info"))])
        self.root.withdraw()  # --play direct mode: the root is never shown
        self.widgets = sys.modules["widgets"]

    def dialog(self):
        return next(w for w in self.root.winfo_children() if isinstance(w, self.widgets.Dialog))

    def test_clicked_button_returns_its_value(self):
        Dialog = self.widgets.Dialog
        buttons = [("A", "a", "primary"), ("B", "b"), ("C", "c", "danger")]
        self.root.after(100, lambda: self.dialog().buttons[1].invoke())
        self.assertEqual(Dialog.ask(self.root, "t", "message", buttons, default=0, cancel="x"), "b")

    def test_enter_is_the_default_and_escape_is_cancel(self):
        Dialog = self.widgets.Dialog
        buttons = [("A", "a", "primary"), ("B", "b")]
        self.root.after(100, lambda: self.dialog().event_generate("<Return>"))
        self.assertEqual(Dialog.ask(self.root, "t", "m", buttons, default=1), "b")
        self.root.after(100, lambda: self.dialog().event_generate("<Escape>"))
        self.assertEqual(Dialog.ask(self.root, "t", "m", buttons, cancel="x"), "x")
        self.root.after(100, lambda: self.dialog().event_generate("<Escape>"))
        self.assertIsNone(Dialog.ask(self.root, "t", "m", buttons))

    def test_yesno_okcancel_and_single_ok(self):
        Dialog = self.widgets.Dialog
        self.root.after(100, lambda: self.dialog().buttons[0].invoke())
        self.assertIs(Dialog.yesno(self.root, "t", "q"), True)
        self.root.after(100, lambda: self.dialog().buttons[1].invoke())
        self.assertIs(Dialog.yesno(self.root, "t", "q"), False)
        self.root.after(100, lambda: self.dialog().event_generate("<Escape>"))
        self.assertIs(Dialog.okcancel(self.root, "t", "q"), False)
        self.root.after(100, lambda: self.dialog().event_generate("<Return>"))
        self.assertIs(Dialog.okcancel(self.root, "t", "q"), True)
        self.root.after(100, lambda: self.dialog().event_generate("<Return>"))
        self.assertIsNone(Dialog.error(self.root, "t", "boom", detail="line\n" * 30))

    def test_copy_button_puts_the_detail_on_the_clipboard(self):
        Dialog = self.widgets.Dialog

        def copy():
            self.dialog().copy_button.invoke()
            self.dialog().finish(None)
        self.root.after(100, copy)
        Dialog.error(self.root, "t", "m", detail="C:\\some\\path\nsecond line")
        self.assertEqual(self.root.clipboard_get(), "C:\\some\\path\nsecond line")

    def test_info_tip_opens_one_popover_at_a_time(self):
        InfoTip, Badge = self.widgets.InfoTip, self.widgets.Badge
        self.root.deiconify()
        self.root.update()
        first = InfoTip(self.root, "first full text", 1.0, ("Segoe UI", 9))
        second = InfoTip(self.root, "second full text", 1.0, ("Segoe UI", 9))
        Badge(self.root, "ALL GAMES", 1.0, ("Segoe UI", 8)).pack()
        first.pack()
        second.pack()
        self.root.update()
        self.addCleanup(InfoTip.dismiss)
        first.open()
        self.assertTrue(first.popover.winfo_exists())
        self.assertEqual(first.popover.winfo_manager(), "place")  # inside the window, not a Toplevel
        self.assertIs(InfoTip.current, first)
        old = first.popover
        second.open()
        self.assertFalse(old.winfo_exists())
        self.assertIsNone(first.popover)
        self.assertTrue(second.popover.winfo_exists())
        InfoTip.dismiss(second)  # a click on its own "?" is left to its handler
        self.assertTrue(second.popover.winfo_exists())
        InfoTip.dismiss(self.root)  # a click elsewhere
        self.assertIsNone(second.popover)
        self.assertIsNone(InfoTip.current)
        first.open(pin=True)
        first.leave()  # pinned: the pointer leaving does not close it
        self.assertIsNotNone(first.popover)
        first.toggle()  # Space / Enter on the focused "?" closes an open one
        self.assertIsNone(first.popover)
        first.toggle()
        self.assertTrue(first.popover.winfo_exists())

    def test_toast_shows_stacks_and_goes_away(self):
        Toast = self.widgets.Toast
        self.assertIsNone(Toast.show(self.root, "hidden root", "ok"))  # direct mode: nothing
        self.root.deiconify()
        self.root.update()
        if not self.root.winfo_viewable():
            self.skipTest("the root cannot be shown here")
        with mock.patch.object(Toast, "DELAY", 150):
            first = Toast.show(self.root, "one", "ok")
            for text in ("two", "three", "four"):
                Toast.show(self.root, text, "error")
            self.assertEqual(len(Toast.stack(self.root)), Toast.MAX)  # the oldest made room
            self.assertFalse(first.winfo_exists())
            end = time.monotonic() + 3
            while Toast.stack(self.root) and time.monotonic() < end:
                self.root.update()
                time.sleep(0.01)
        self.assertEqual(Toast.stack(self.root), [])


if __name__ == "__main__":
    unittest.main()
