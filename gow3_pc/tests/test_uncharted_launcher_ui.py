"""Headless smoke test of launcher/uncharted_launcher.pyw (a real Tk root, withdrawn).

Run: py -m unittest test_uncharted_launcher_ui  (from gow3_pc/tests; needs Tk and Pillow).
"""
import importlib.machinery
import importlib.util
import os
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
class OptionAvailabilityTest(unittest.TestCase):
    def test_rules_per_upscaler(self):
        enabled = lambda upscaler, ok: {k: v[0] for k, v in  # noqa: E731
                                        uncharted_launcher.option_availability({"upscaler": upscaler}, ok).items()}
        for ok in (True, False):
            self.assertEqual(enabled("off", ok), {"preset": False, "dlss_model": False, "frame_gen": False})
            self.assertEqual(enabled("taa", ok), {"preset": False, "dlss_model": False, "frame_gen": ok})
            for fsr in ("fsr3", "fsr4"):
                self.assertEqual(enabled(fsr, ok), {"preset": True, "dlss_model": False, "frame_gen": ok})
            self.assertEqual(enabled("dlss", ok), {"preset": True, "dlss_model": True, "frame_gen": ok})

    def test_reasons_only_while_disabled(self):
        result = uncharted_launcher.option_availability({"upscaler": "off"}, False)
        self.assertEqual(result["preset"][1], "Needs FSR or DLSS under Anti-aliasing.")
        self.assertEqual(result["dlss_model"][1], "Needs DLSS under Anti-aliasing.")
        self.assertEqual(result["frame_gen"][1], "Needs an anti-aliasing mode (TAA, FSR or DLSS).")
        result = uncharted_launcher.option_availability({"upscaler": "taa"}, False)
        self.assertEqual(result["frame_gen"][1], "Needs NVIDIA's nvngx_dlssg.dll: add it under Ready to play on Home.")
        self.assertEqual(uncharted_launcher.option_availability({"upscaler": "dlss"}, True),
                         {k: (True, None) for k in ("preset", "dlss_model", "frame_gen")})
        self.assertFalse(uncharted_launcher.option_availability({}, True)["preset"][0])  # the default is off


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

    def test_apply_to_all_games_copies_display_settings_but_not_controls(self):
        launcher, mod = self.launcher, uncharted_launcher
        launcher.ask_yesno = lambda *args, **kwargs: True
        launcher.select_game("u1")
        others = [k for k in mod.GAME_KEYS if k != "u1"]
        mod.write_ini("u2", {"key_map": "cross=Q"})
        launcher.change("fps_limit", "90")
        launcher.change_resolution("2560x1440")
        launcher.toast = lambda *args, **kwargs: None
        launcher.process = object()  # a game runs: nothing is copied
        launcher.warn = lambda *args, **kwargs: None
        launcher.apply_to_all_games()
        self.assertEqual(mod.read_ini("u2")["fps_limit"], mod.INI_DEFAULTS["fps_limit"])
        launcher.process = None
        launcher.apply_to_all_games()
        for key in others:
            self.assertEqual(mod.read_ini(key)["fps_limit"], "90")
            self.assertEqual(launcher.config["games"][key]["resolution"], "2560x1440")
            self.assertEqual(launcher.config["games"][key]["aspect"], launcher.config["games"]["u1"]["aspect"])
        self.assertEqual(mod.read_ini("u2")["key_map"], "cross=Q")

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

    def test_play_or_stop_asks_before_killing_a_running_game(self):
        launcher = self.launcher
        launcher.process = SimpleNamespace(pid=1, poll=lambda: None)
        self.addCleanup(setattr, launcher, "process", None)
        for answer in (False, True):
            with mock.patch.object(launcher, "ask_yesno", return_value=answer) as ask, \
                    mock.patch.object(launcher, "stop") as stop:
                launcher.play_or_stop()
            ask.assert_called_once()
            self.assertEqual(stop.called, answer)

    def test_dropdown_move_clamps_and_notifies(self):
        calls = []
        dropdown = uncharted_launcher.Dropdown(self.root, [("a", "A"), ("b", "B"), ("c", "C")], "b", calls.append,
                                               ("Segoe UI", 9))
        dropdown.move(1)
        dropdown.move(1)  # already the last: nothing
        dropdown.move(-1)
        dropdown.move(-5)
        dropdown.move(-1)
        self.assertEqual(calls, ["c", "b", "a"])
        self.assertEqual(dropdown.value, "a")

    def test_selecting_another_game_cancels_a_pending_capture(self):
        launcher = self.launcher
        launcher.select_game("u1", save=False)
        launcher.start_capture("cross", 0)
        self.assertIsNotNone(launcher.capturing)
        launcher.select_game("u2", save=False)
        self.assertIsNone(launcher.capturing)

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

    def test_options_that_do_nothing_are_greyed_out_and_wake_up_without_losing_values(self):
        launcher, mod = self.launcher, uncharted_launcher
        game = launcher.config["game"]
        with mock.patch.object(mod, "frame_gen_available", lambda: True):
            launcher.change("preset", "3")
            launcher.change("dlss_model", "m")
            launcher.change("upscaler", "off")
            launcher.refresh_controls()  # the widgets show the ini
            self.root.update()
            preset, model, frame_gen = (launcher.widgets[k] for k in ("preset", "dlss_model", "frame_gen"))
            self.assertEqual([preset.enabled, model.enabled, frame_gen.enabled], [False, False, False])
            self.assertEqual(launcher.row_notes["preset"].winfo_manager(), "pack")
            self.assertIn("FSR or DLSS", launcher.row_notes["preset"].cget("text"))
            # A click on a greyed-out choice neither calls the command nor reaches the ini.
            preset.click("1")  # what a click on the "Quality" item calls
            self.assertEqual((preset.value, mod.read_ini(game)["preset"]), ("3", "3"))
            self.assertNotEqual(preset.labels["3"].cget("bg"), mod.ACCENT)  # the selected one is not orange now
            launcher.change("upscaler", "dlss")
            self.root.update()
            self.assertEqual([preset.enabled, model.enabled, frame_gen.enabled], [True, True, True])
            self.assertEqual(launcher.row_notes["preset"].winfo_manager(), "")
            self.assertEqual((launcher.ini["preset"], launcher.ini["dlss_model"]), ("3", "m"))
            self.assertEqual((preset.value, model.value), ("3", "m"))
            self.assertNotEqual(launcher.game_settings().get("preset"), "3")  # the Home preset name, untouched
            preset.click("1")
            self.assertEqual(mod.read_ini(game)["preset"], "1")  # enabled again: the click goes through

    def test_missing_frame_generation_dll_is_the_reason_once_an_upscaler_is_on(self):
        launcher, mod = self.launcher, uncharted_launcher
        with mock.patch.object(mod, "frame_gen_available", lambda: False):
            launcher.change("upscaler", "taa")
            self.assertFalse(launcher.widgets["frame_gen"].enabled)
            self.assertIn("nvngx_dlssg.dll", launcher.row_notes["frame_gen"].cget("text"))
            launcher.change("upscaler", "off")
            self.assertIn("anti-aliasing mode", launcher.row_notes["frame_gen"].cget("text"))

    def test_following_the_in_game_menu_wakes_the_options(self):
        launcher, mod = self.launcher, uncharted_launcher
        game = launcher.config["game"]
        launcher.change("upscaler", "off")
        self.assertFalse(launcher.widgets["preset"].enabled)
        mod.write_ini(game, {"upscaler": "fsr3"})  # what the in-game menu does
        stamp = mod.ini_file(game).stat().st_mtime_ns + 1_000_000
        os.utime(mod.ini_file(game), ns=(stamp, stamp))
        launcher.follow_ini()
        self.assertEqual(launcher.widgets["upscaler"].value, "fsr3")
        self.assertTrue(launcher.widgets["preset"].enabled)
        self.assertFalse(launcher.widgets["dlss_model"].enabled)

    def test_toggle_and_dropdown_ignore_clicks_while_disabled(self):
        calls = []
        toggle = uncharted_launcher.Toggle(self.root, 1.0, False, calls.append)
        toggle.set_enabled(False)
        toggle.click()
        dropdown = uncharted_launcher.Dropdown(self.root, [("a", "A"), ("b", "B")], "a", calls.append, ("Segoe UI", 9))
        dropdown.set_enabled(False)
        with mock.patch.object(tk.Menu, "tk_popup", lambda *a: calls.append("menu")):
            dropdown.open()
        dropdown.set("b")  # programmatic updates still render
        self.assertIn("B", dropdown.cget("text"))
        self.assertEqual(calls, [])
        toggle.set_enabled(True)
        toggle.click()
        self.assertEqual(calls, [True])

    def test_keyboard_arrows_change_a_choice_and_disabled_ones_skip_tab(self):
        launcher, mod = self.launcher, uncharted_launcher
        choice = launcher.widgets["upscaler"]
        first, second = list(choice.labels)[:2]
        launcher.change("upscaler", first)
        self.assertEqual(str(choice.cget("takefocus")), "1")
        choice.focus_set()
        choice.move(1)  # what Right does
        self.assertEqual(choice.value, second)
        self.assertEqual(mod.read_ini(launcher.config["game"])["upscaler"], second)
        choice.move(to=0)  # Home
        self.assertEqual(mod.read_ini(launcher.config["game"])["upscaler"], first)
        choice.move(-1)  # Left on the first one: nothing to change
        self.assertEqual(choice.value, first)
        choice.set_enabled(False)
        self.assertEqual(str(choice.cget("takefocus")), "0")

    def test_shortcuts_select_the_game_and_cycle_the_sections(self):
        launcher = self.launcher
        launcher.shortcut(lambda: launcher.select_game("u2"))()  # Ctrl+2
        self.assertEqual(launcher.config["game"], "u2")
        launcher.show_tab("home", save=False)
        launcher.step_section(1)  # Ctrl+Tab
        self.assertEqual(launcher.section, "display")
        launcher.step_section(-1)
        launcher.step_section(-1)  # Ctrl+Shift+Tab wraps to the last
        self.assertEqual(launcher.section, "system")

    def test_tab_is_a_binding_while_a_slot_captures(self):
        launcher = self.launcher
        launcher.capturing, launcher.capture_armed = ("jump", 0), True
        seen = []
        with mock.patch.object(launcher, "capture_key", lambda event: seen.append(event.keysym) or "break"):
            self.assertEqual(launcher.on_key(SimpleNamespace(keysym="Tab")), "break")  # root's <Tab>
            step = launcher.shortcut(lambda: seen.append("section"), tab=True)  # Ctrl+Tab
            self.assertEqual(step(SimpleNamespace(keysym="Tab")), "break")
            self.assertIsNone(launcher.shortcut(lambda: seen.append("game"))(SimpleNamespace(keysym="2")))
        self.assertEqual(seen, ["Tab", "Tab"])
        launcher.capturing, launcher.capture_armed = None, False
        self.assertIsNone(launcher.on_key(SimpleNamespace(keysym="Tab")))  # not capturing: Tk moves the focus

    def test_controller_nav_ignores_held_buttons_then_moves_and_steps(self):
        launcher = self.launcher
        pads = SimpleNamespace(sdl=True, down={"dpdown"}, held=lambda: set(pads.down),
                               axes=lambda: {"lx": 0.0, "ly": 0.0})
        launcher.gamepads = pads
        nav = uncharted_launcher.PadNav(launcher)
        nav.stop()
        target = mock.Mock()
        focus = SimpleNamespace(winfo_toplevel=lambda: self.root, tk_focusNext=lambda: target,
                                tk_focusPrev=lambda: None)
        with mock.patch.object(nav, "window_focused", return_value=True), \
                mock.patch.object(self.root, "focus_get", return_value=focus), \
                mock.patch.object(launcher, "step_section") as step_section, \
                mock.patch.object(launcher, "play_or_stop") as play:
            nav.step(0.0)  # Down was held when the nav woke up: ignored
            nav.step(0.1)
            target.focus_set.assert_not_called()
            pads.down = set()
            nav.step(0.2)
            pads.down = {"dpdown"}
            nav.step(0.3)
            self.assertEqual(target.focus_set.call_count, 1)
            nav.step(0.4)  # held: no repeat before 400 ms
            self.assertEqual(target.focus_set.call_count, 1)
            nav.step(0.8)
            self.assertEqual(target.focus_set.call_count, 2)
            pads.down = {"rightshoulder", "start"}
            nav.step(1.0)
            step_section.assert_called_once_with(1)
            play.assert_called_once_with()
            launcher.process = object()  # a game runs: the pad is the game's
            pads.down = set()
            nav.step(1.1)
            pads.down = {"start", "rightshoulder", "dpdown"}
            nav.step(1.2)
            launcher.process = None
        step_section.assert_called_once_with(1)
        play.assert_called_once_with()
        self.assertEqual(target.focus_set.call_count, 2)

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

    def test_current_dialog_takes_the_controller_buttons(self):
        Dialog = self.widgets.Dialog
        seen = []

        def cancel():
            seen.append(Dialog.current is self.dialog())
            self.dialog().step_button(1)
            seen.append(self.dialog().default_button is self.dialog().buttons[1])
            self.dialog().press_cancel()
        self.root.after(100, cancel)
        self.assertEqual(Dialog.ask(self.root, "t", "m", [("A", "a"), ("B", "b")], cancel="x"), "x")
        self.assertEqual(seen, [True, True])
        self.assertIsNone(Dialog.current)

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
