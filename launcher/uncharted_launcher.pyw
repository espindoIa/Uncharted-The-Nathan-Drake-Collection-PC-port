#!/usr/bin/env python3
"""Uncharted: The Nathan Drake Collection - launcher (Windows, Tkinter + Pillow).

Pick a game, set display and performance options (presets for low-end and high-end PCs) and play.
Runtime options are kept in analysis/uncharted-user/probe.ini, the same file the in-game menu
(Insert / R3+L2) reads and writes; launcher-only options in %APPDATA%/Uncharted Launcher (under
Wine/Proton in ~/.local/share/uncharted-launcher, the same for every prefix).
The game is started through tools/probe_uncharted.py (preparation, code patches, Collection game
switches), with the options below passed as environment variables.
"""
import ctypes
import json
import os
from pathlib import Path
import queue
import re
import shutil
import struct
import subprocess
import sys
import threading
import time
import tkinter as tk
import webbrowser
from tkinter import font as tkfont, messagebox

from PIL import Image, ImageDraw, ImageOps, ImageTk

import updater  # launcher/updater.py: updates from the GitHub releases
from widgets import (ACCENT, ACCENT_HI, BG, DIM, GOOD, HERO_DARK, HERO_GRID, HERO_LIGHT, HOVER_LINE, INK, LINE, MUTED, PANEL, PANEL_HI, STOP_RED, TEXT, Badge, Chip, Dialog, Dropdown, InfoTip, ScrollBar, Segmented, Toast, Toggle, copy_text, on_activate, on_keys, round_rect)  # launcher/widgets.py

ROOT = Path(__file__).resolve().parent.parent
ASSETS = ROOT / "launcher" / "assets"
# Cells of assets/icons.png, line icons drawn in VectorCraft (source: assets/src/icons.svg); the sheet
# is white on transparent so each use tints it. assets/emblem.png is the sidebar's compass.
GLYPHS = ("home", "display", "performance", "controls", "gameplay", "system", "link")
# The Collection's regions the port is tested with: US (CUSA02320, the fork's own copy, with update
# 1.02) and Europe (CUSA02344). Their 1.02 executables are the same; every patch finds its bytes in both.
GAME_IDS = ("CUSA02320", "CUSA02344")
DEFAULT_GAME_DIR = next((ROOT / "game" / title / sub for title in GAME_IDS for sub in ("uroot", "")
                         if (ROOT / "game" / title / sub / "eboot.bin").is_file()),
                        ROOT / "game" / GAME_IDS[0])
STARTER_EXE = ROOT / "Uncharted Launcher.exe"  # the packaged build's starter (tools/package_windows.py)
USER_DIR = ROOT / "analysis" / "uncharted-user"
INI_FILE = USER_DIR / "probe.ini"
RUN_SCRIPT = ROOT / "tools" / "probe_uncharted.py"
PREFIX_CONFIG_FILE = Path(os.environ.get("APPDATA", Path.home())) / "Uncharted Launcher" / "launcher.json"


def wine_kernel32():
    """kernel32 when the launcher runs under Wine (Proton, Lutris, CrossOver), else None."""
    try:
        if hasattr(ctypes.windll.ntdll, "wine_get_version"):
            return ctypes.windll.kernel32
    except (AttributeError, OSError):
        pass
    return None


WINE = wine_kernel32()


def slow_drive_reason(path):
    """Why the drive holding `path` makes the games freeze, or None (20 Oct 2026). On a
    laptop the port and game on an external USB drive (an NVMe SSD in a USB case, exFAT) froze
    for 0.2-0.8 s dozens of times a minute while the port wrote its shader cache, logs and saves
    (Windows writes to such drives without a cache); on the internal NVMe drive: no freeze over
    100 ms. Reports USB, SD/MMC and removable drives and FAT/exFAT volumes; None elsewhere, under
    Wine and for network paths."""
    if os.name != "nt" or WINE:
        return None
    drive = os.path.splitdrive(str(Path(path).resolve()))[0]
    if len(drive) != 2 or drive[1] != ":":
        return None
    kernel32 = ctypes.windll.kernel32
    reasons = []
    fs_name = ctypes.create_unicode_buffer(32)
    if kernel32.GetVolumeInformationW(drive + "\\", None, 0, None, None, None, fs_name, len(fs_name)):
        if fs_name.value.upper() in ("EXFAT", "FAT32", "FAT"):
            reasons.append(f"{fs_name.value}-formatted")
    if kernel32.GetDriveTypeW(drive + "\\") == 2:  # DRIVE_REMOVABLE
        reasons.append("removable")
    # The bus of the disk (IOCTL_STORAGE_QUERY_PROPERTY, StorageDeviceProperty): USB enclosures
    # report themselves as fixed drives.
    kernel32.CreateFileW.restype = ctypes.c_void_p
    handle = kernel32.CreateFileW("\\\\.\\" + drive, 0, 3, None, 3, 0, None)  # share read|write, OPEN_EXISTING
    if handle and handle != ctypes.c_void_p(-1).value:
        try:
            query = struct.pack("<II4x", 0, 0)  # StorageDeviceProperty, PropertyStandardQuery
            out = ctypes.create_string_buffer(1024)
            returned = ctypes.c_ulong(0)
            if kernel32.DeviceIoControl(ctypes.c_void_p(handle), 0x2D1400, query, len(query), out,
                                        len(out), ctypes.byref(returned), None) and returned.value >= 32:
                bus = struct.unpack_from("<I", out.raw, 28)[0]
                names = {7: "USB", 12: "SD card", 13: "MMC card", 4: "FireWire"}
                if bus in names:
                    reasons.insert(0, names[bus])
        finally:
            kernel32.CloseHandle(ctypes.c_void_p(handle))
    if not reasons:
        return None
    return f"{drive} is a {', '.join(dict.fromkeys(reasons))} drive"


def _wine_string(pointer, wide):
    """A string Wine returned in memory of the process heap, which is freed."""
    if not pointer:
        return None
    try:
        return ctypes.wstring_at(pointer) if wide else ctypes.string_at(pointer).decode("utf-8", "replace")
    finally:
        WINE.GetProcessHeap.restype = ctypes.c_void_p
        WINE.HeapFree.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_void_p]
        WINE.HeapFree(WINE.GetProcessHeap(), 0, pointer)


def unix_path(path):
    """The Linux path of a Windows path under Wine, else None."""
    if not WINE:
        return None
    function = WINE.wine_get_unix_file_name
    function.argtypes, function.restype = [ctypes.c_wchar_p], ctypes.c_void_p
    return _wine_string(function(str(path)), wide=False)


def windows_path(path):
    """The Windows path of a Linux path under Wine (in this prefix's drive letters), else None."""
    if not WINE or not path:
        return None
    function = WINE.wine_get_dos_file_name
    function.argtypes, function.restype = [ctypes.c_char_p], ctypes.c_void_p
    return _wine_string(function(path.encode("utf-8")), wide=True)


def shared_config_file():
    """Under Wine: launcher.json in the Linux user's data folder. Each Steam shortcut has a prefix
    (and an %APPDATA%) of its own, so a game started from its shortcut (--play) would not see the
    launcher's options; this folder is the same for all of them, and outside the port's folder."""
    home = os.environ.get("HOME", "")
    data = os.environ.get("XDG_DATA_HOME") or (home + "/.local/share" if home.startswith("/") else "")
    folder = windows_path(data) if data.startswith("/") else None
    if not folder and os.environ.get("WINEHOMEDIR", "").startswith("\\??\\"):
        folder = str(Path(os.environ["WINEHOMEDIR"][4:]) / ".local" / "share")  # set by Wine itself
    return Path(folder) / "uncharted-launcher" / "launcher.json" if folder else None


CONFIG_FILE = (WINE and shared_config_file()) or PREFIX_CONFIG_FILE
NO_WINDOW = getattr(subprocess, "CREATE_NO_WINDOW", 0)
APP_TITLE = "Uncharted: The Nathan Drake Collection"
# The fork's name in the window title and the hero.
EDITION = "Expedition Edition"
FORK_URL = "https://github.com/espindoIa/Uncharted-The-Nathan-Drake-Collection-PC-port"
# Credits shown in the System section: the port this fork builds on and the anti-aliasing technologies.
CREDITS = ("Credits: based on BilalReloaded's Uncharted PC port (github.com/bc160201983), GPL. DLSS and DLAA "
           "use NVIDIA DLSS technology. NVIDIA, RTX and DLSS are trademarks of NVIDIA Corporation. AMD "
           "FidelityFX Super Resolution (FSR 3.1, FSR 4) by AMD. Uncharted camera motion for anti-aliasing "
           "after SuperMedo's Uncharted port.")
# Links at the bottom of the sidebar: label, address, color, hover color.
LINKS = (("GitHub", FORK_URL, "#18262f", "#22343f"),)

GAMES = [
    {"key": "u1", "number": "1", "title": "DRAKE'S FORTUNE", "name": "Uncharted: Drake's Fortune", "subtitle": "Uncharted 1",
     "exe": "eboot.bin", "log": "uncharted"},
    {"key": "u2", "number": "2", "title": "AMONG THIEVES", "name": "Uncharted 2: Among Thieves", "subtitle": "Uncharted 2",
     "exe": "big2-ps4_Shipping.elf", "log": "big2-ps4_Shipping"},
    {"key": "u3", "number": "3", "title": "DRAKE'S DECEPTION", "name": "Uncharted 3: Drake's Deception", "subtitle": "Uncharted 3",
     "exe": "big3-ps4_Shipping.elf", "log": "big3-ps4_Shipping"},
]
# The sidebar's sections (internal name, label). launcher.json's "tab" keeps the name; names of
# launchers before 24 Oct 2026 map to their section (OLD_SECTIONS), anything unknown opens Home.
SECTIONS = (("home", "Home"), ("display", "Display"), ("performance", "Performance"), ("controls", "Controls"),
            ("gameplay", "Gameplay"), ("system", "System"))
OLD_SECTIONS = {"setup": "home", "cheats": "gameplay", "advanced": "system"}


def section_name(name):
    name = OLD_SECTIONS.get(name, name)
    return name if name in dict(SECTIONS) else "home"

# At most 120: faster, the game's movement timing breaks (Drake's Fortune at 140-160 FPS: running
# slows, Nate fell through the world). The runtime caps older, faster settings at 120 too.
MAX_FPS = 120
FPS_LIMITS = [(30, "30"), (40, "40"), (45, "45"), (50, "50"), (55, "55"), (60, "60"), (75, "75"),
              (90, "90"), (100, "100"), (MAX_FPS, str(MAX_FPS))]
# 22 Oct 2026: with DLSS Frame Generation on, the runtime's limit counts the frames shown and may
# go above 120 (the game still runs at half that); without it the runtime caps at 120. Listed only
# when Frame Generation is available (see frame_gen_available).
FG_FPS_LIMITS = [(v, str(v)) for v in (144, 160, 165, 180, 200, 240)]

# Runtime options in probe.ini (key -> default) and launcher-only ones.
INI_DEFAULTS = {
    "display_mode": "windowed", "vsync": "1", "fps_limit": "60", "frames_queued": "1",
    "async_shaders": "1", "fsr1": "0", "rcas": "1", "rcas_strength": "75", "show_fps": "1",
    "upscaler": "off", "preset": "0", "dlss_model": "k", "gpu_id": "-1", "anisotropy": "16",
    "reflex": "0", "frame_gen": "0",
    # The performance counter (Display section): the items on its line, its corner, layout, text size
    # and background opacity. The runtime's defaults; the in-game Overlay tab writes them too.
    # Items as the runtime starts (DefaultItems: FPS, frame time, upscaler) until the game has
    # written its ini once.
    "overlay_fps": "1", "overlay_frametime": "1", "overlay_gpu": "0", "overlay_cpu": "0",
    "overlay_ram": "0", "overlay_vram": "0", "overlay_upscaler": "1", "overlay_corner": "1",
    "overlay_layout": "0", "overlay_scale": "100", "overlay_opacity": "50",
}
# FSR/DLSS quality modes (the runtime's preset key, also in the in-game menu): 0 draws the scene at
# the render resolution (Native AA / DLAA); the others draw it smaller and rebuild it at the render
# resolution, which is then their output.
UPSCALE_MODES = [("0", "Native / DLAA"), ("5", "Ultra Quality"), ("1", "Quality"), ("2", "Balanced"),
                 ("3", "Performance"),
                 ("4", "Ultra Perf.")]
# DLSS transformer models (the runtime's dlss_model key, also in the in-game menu). NVIDIA's
# defaults: K for DLAA, Quality and Balanced, M for Performance, L for Ultra Performance. 19 Oct,
# U3 at 4K on an RTX 5070 (GOW3_GPU_PROFILE): DLAA K 2.2 ms, M 7.9 ms a frame; Performance K
# 2.0 ms, M 2.9 ms (NVIDIA's programming guide: 2.11 and 2.98 ms).
DLSS_MODELS = [("j", "Model J"), ("k", "Model K"), ("l", "Model L"), ("m", "Model M")]


# NVIDIA's nvngx_dlss.dll is not shipped (its license does not fit the port's GPL): the player
# downloads it from NVIDIA's DLSS repository and the launcher copies it next to the runtime after
# checking NVIDIA's signature. gow3_dlss.dll (the bridge) is shipped.
NGX_DLL = "nvngx_dlss.dll"
NGX_FG_DLL = "nvngx_dlssg.dll"  # DLSS Frame Generation, same repository and rules
NGX_DOWNLOAD_URL ="https://github.com/NVIDIA/DLSS/tree/main/lib/Windows_x86_64/rel"


def dlss_bridge_present():
    return (ROOT / "gow3_pc" / "out" / "gow3_dlss.dll").is_file()


def dlss_available():
    return dlss_bridge_present() and (ROOT / "gow3_pc" / "out" / NGX_DLL).is_file()


def frame_gen_available():
    return dlss_available() and (ROOT / "gow3_pc" / "out" / NGX_FG_DLL).is_file()


DEPENDENT_OPTIONS = ("preset", "dlss_model", "frame_gen")


def option_availability(ini, frame_gen_ok):
    """24 Oct 2026: which options do something with the chosen anti-aliasing: key -> (enabled, reason
    or None). Only a display state: the stored values are never touched, so they are back as they
    were when the setting they depend on changes again."""
    upscaler = ini.get("upscaler", INI_DEFAULTS["upscaler"])
    result = {"preset": (upscaler in ("fsr3", "fsr4", "dlss"), "Needs FSR or DLSS under Anti-aliasing."),
              "dlss_model": (upscaler == "dlss", "Needs DLSS under Anti-aliasing.")}
    if upscaler == "off":
        result["frame_gen"] = (False, "Needs an anti-aliasing mode (TAA, FSR or DLSS).")
    elif not frame_gen_ok:
        result["frame_gen"] = (False, "Needs NVIDIA's nvngx_dlssg.dll: add it under Ready to play on Home.")
    else:
        result["frame_gen"] = (True, None)
    return {key: (enabled, None if enabled else reason) for key, (enabled, reason) in result.items()}


def fps_limits():
    return FPS_LIMITS + FG_FPS_LIMITS if frame_gen_available() else FPS_LIMITS


def ngx_signature_ok(path):
    """Whether `path` carries a valid Authenticode signature by NVIDIA Corporation."""
    # 20 Oct 2026 audit: the path goes in the environment, not into the script text; a ' in a folder
    # name broke parsing (genuine DLL called unsigned) and a crafted name could run commands.
    script = ("$s = Get-AuthenticodeSignature -LiteralPath $env:NGX_DLL; "
              "if ($s.Status -eq 'Valid' -and $s.SignerCertificate.Subject -like '*O=NVIDIA Corporation*') "
              "{ exit 0 } else { exit 1 }")
    try:
        result = subprocess.run(["powershell", "-NoProfile", "-NonInteractive", "-Command", script],
                                capture_output=True, timeout=60, env={**os.environ, "NGX_DLL": str(path)},
                                creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    except (OSError, subprocess.TimeoutExpired):
        return False
    return result.returncode == 0


# Cheats (Gameplay section): name, title, hint. tools/probe_uncharted.py turns on the games' own
# switches for them (CHEATS there).
CHEATS = (("invincible", "Invincible", "Nate takes no damage. All three games."),
          ("ammo", "Infinite ammo", "Ammo never runs out. All three games."),
          ("noreload", "No reload", "The magazine never empties. Uncharted 2 and 3."),
          ("grenades", "Infinite grenades", "Grenades never run out. Uncharted 3."),
          ("onehit", "One-hit kills", "Enemies go down with one hit. Uncharted 3."))
HINT_MAX = 110  # a settings row's hint longer than this needs a `short` line (<= SHORT_MAX) and a ? with the full text
SHORT_MAX = 100
ALL_GAMES = "ALL GAMES"  # the badge on settings that apply to all three games (launcher config, not a game's ini)
LAUNCHER_DEFAULTS = {"game": "u1", "diagnostics": True, "game_dir": "", "check_updates": True, "language": "1",
                     "unlock": False}

# The PS4's system language (its id, as sceSystemServiceParamGetInt reports it) for the 22 languages
# the Collection has text for (all three games; text1/ps4/<n>.common in core.psarc). Each game's main
# menu then offers it next to English (Square). Spoken languages depend on the copy of the game:
# its sound1 speech packs (SPEECH_PACKS).
LANGUAGES = [("1", "English (US)"), ("18", "English (UK)"), ("2", "Français"), ("4", "Deutsch"),
             ("5", "Italiano"), ("3", "Español (España)"), ("20", "Español (Latinoamérica)"),
             ("7", "Português (Portugal)"), ("17", "Português (Brasil)"), ("6", "Nederlands"),
             ("14", "Dansk"), ("15", "Norsk"), ("13", "Svenska"), ("12", "Suomi"), ("16", "Polski"),
             ("23", "Čeština"), ("25", "Ελληνικά"), ("8", "Русский"), ("19", "Türkçe"),
             ("0", "日本語"), ("9", "한국어"), ("10", "中文 (繁體)")]
SPEECH_PACKS = {"english": ("1", "18"), "french": ("2",), "german": ("4",), "italian": ("5",),
                "spanish": ("3", "20"), "portuguese": ("7", "17"), "dutch": ("6",), "danish": ("14",),
                "norwegian": ("15",), "swedish": ("13",), "finnish": ("12",), "polish": ("16",),
                "czech": ("23",), "greek": ("25",), "russian": ("8",), "turkish": ("19",),
                "japanese": ("0",), "korean": ("9",), "chinese": ("10",)}


def voice_languages(game_dir):
    """Language ids with speech in this copy of the game (its *_ps4.psarc speech packs)."""
    ids = set()
    for pattern in ("u*data/build/main/sound1/speech/*_ps4.psarc",
                    "u*data/build/main/sound1/streams/speech/*_ps4.psarc"):
        for pack in Path(game_dir).glob(pattern) if game_dir else ():
            ids.update(SPEECH_PACKS.get(pack.name.removesuffix("_ps4.psarc").lower(), ()))
    return ids
GAME_KEYS = ("u1", "u2", "u3")
GAME_SETTING_DEFAULTS = {"preset": "medium", "resolution": "1920x1080", "aspect": "16:9", "multithreaded": True,
                         "save_profile": "1",
                         "async_readback": False, "controls": None, "mouse": True, "mouse_sens": "1.0",
                         "mouse_invert": False, "prompts": "auto", "mouse_aim_sens": "1.0",
                         "mouse_vertical": "1.0", "mouse_invert_x": False, "vibration": True}

# Keyboard and mouse controls (runtime_pad.c): action -> up to two bindings, "key:<SDL key name>"
# or "mouse:<left|middle|right|x1|x2|wheel_up|wheel_down>". Stick directions take keys only.
CONTROL_GROUPS = (
    ("MOVEMENT", (("ls_up", "Move forward"), ("ls_down", "Move back"), ("ls_left", "Move left"),
                  ("ls_right", "Move right"), ("rs_up", "Look up"), ("rs_down", "Look down"),
                  ("rs_left", "Look left"), ("rs_right", "Look right"))),
    ("WEAPONS (D-PAD)", (("left", "Left"), ("right", "Right"), ("up", "Up"), ("down", "Down"))),
    ("ACTIONS", (("cross", "Jump, climb"), ("circle", "Cover, roll, drop"),
                 ("square", "Melee"), ("triangle", "Reload, pick up"),
                 ("l2", "Aim"), ("r2", "Fire"), ("l1", "Grenade aim"),
                 ("r1", "Grenade throw"), ("l3", "Left stick press"), ("r3", "Right stick press"),
                 ("options", "Pause menu"), ("touchpad_left", "Touch left"),
                 ("touchpad_right", "Touch right"))),
)
# Controllers. The controller button of each action, as SDL names it (runtime_pad.c's default
# controller map; GOW3_PAD_MAP takes these names). Buttons can be rebound; the sticks, triggers
# and the touchpad's left click (Back/View with LB held) cannot.
PAD_DEFAULTS = {"cross": "a", "circle": "b", "square": "x", "triangle": "y", "l1": "leftshoulder",
                "r1": "rightshoulder", "l3": "leftstick", "r3": "rightstick", "options": "start",
                "touchpad_right": "back", "up": "dpup", "down": "dpdown", "left": "dpleft", "right": "dpright"}
PAD_FIXED = {"l2": "lefttrigger", "r2": "righttrigger", "touchpad_left": "leftshoulder+back",
             "ls_up": "lstick", "ls_down": "lstick", "ls_left": "lstick", "ls_right": "lstick",
             "rs_up": "rstick", "rs_down": "rstick", "rs_left": "rstick", "rs_right": "rstick"}
# SDL_GamepadButton order (SDL3).
SDL_BUTTONS = ("a", "b", "x", "y", "back", "guide", "start", "leftstick", "rightstick", "leftshoulder",
               "rightshoulder", "dpup", "dpdown", "dpleft", "dpright", "misc1", "paddle1", "paddle2",
               "paddle3", "paddle4", "touchpad")
PAD_NAMES = {  # (Xbox, PlayStation): the slot's text where there is no icon, and hints
    "a": ("A", "Cross"), "b": ("B", "Circle"), "x": ("X", "Square"), "y": ("Y", "Triangle"),
    "back": ("View", "Share"), "guide": ("Guide", "PS"), "start": ("Menu", "Options"),
    "leftstick": ("LS", "L3"), "rightstick": ("RS", "R3"), "leftshoulder": ("LB", "L1"),
    "rightshoulder": ("RB", "R1"), "lefttrigger": ("LT", "L2"), "righttrigger": ("RT", "R2"),
    "dpup": ("D-pad up", "D-pad up"), "dpdown": ("D-pad down", "D-pad down"),
    "dpleft": ("D-pad left", "D-pad left"), "dpright": ("D-pad right", "D-pad right"),
    "misc1": ("Share", "Mute"), "paddle1": ("P1", "P1"), "paddle2": ("P2", "P2"), "paddle3": ("P3", "P3"),
    "paddle4": ("P4", "P4"), "touchpad": ("Touchpad", "Touchpad"), "lstick": ("L stick", "L stick"),
    "rstick": ("R stick", "R stick"),
}


def merged_pad_controls(pad_controls):
    merged = dict(PAD_DEFAULTS)
    for action, button in (pad_controls or {}).items():
        if action in merged:
            merged[action] = button or None
    return merged


def pad_map(pad_controls):
    """GOW3_PAD_MAP for a game's controller bindings ("" when they are the defaults)."""
    merged = merged_pad_controls(pad_controls)
    if merged == PAD_DEFAULTS:
        return ""
    return ",".join(f"{action}={button or ''}" for action, button in merged.items())


def _pad_font(size):
    from PIL import ImageFont
    for name in ("segoeuib.ttf", "arialbd.ttf", "DejaVuSans-Bold.ttf"):
        try:
            return ImageFont.truetype(name, size)
        except OSError:
            continue
    return ImageFont.load_default()


def pad_icon(button, style, height):
    """A controller button's icon (PIL RGBA image, `height` pixels tall): Xbox or PlayStation."""
    s = 4  # drawn 4x, then scaled down (smooth edges)
    H = height * s
    ps = style == "playstation"
    body, rim, ink = (32, 32, 34, 255), (150, 150, 150, 255), (240, 240, 240, 255)
    if "+" in button:  # a combination: the icons side by side with a plus
        parts = [pad_icon(b, style, height) for b in button.split("+")]
        plus_w = height // 2
        img = Image.new("RGBA", (sum(i.width for i in parts) + plus_w * (len(parts) - 1), height))
        x = 0
        for i, part in enumerate(parts):
            img.alpha_composite(part, (x, 0))
            x += part.width
            if i + 1 < len(parts):
                d = ImageDraw.Draw(img)
                cx, cy, r = x + plus_w // 2, height // 2, max(2, height // 6)
                d.line((cx - r, cy, cx + r, cy), fill=ink, width=max(1, height // 12))
                d.line((cx, cy - r, cx, cy + r), fill=ink, width=max(1, height // 12))
                x += plus_w
        return img

    def canvas(width_factor):
        W = int(H * width_factor)
        return Image.new("RGBA", (W, H)), W

    def finish(img):
        return img.resize((max(1, img.width // s), height), Image.LANCZOS)

    def text(d, W, label, color, size_factor=0.55, dy=0):
        font = _pad_font(max(8, int(H * size_factor)))
        box = d.textbbox((0, 0), label, font=font)
        d.text(((W - (box[2] - box[0])) / 2 - box[0], (H - (box[3] - box[1])) / 2 - box[1] + dy), label,
               font=font, fill=color)

    line = max(2, H // 12)
    if button in ("a", "b", "x", "y"):
        img, W = canvas(1.0)
        d = ImageDraw.Draw(img)
        d.ellipse((line, line, W - line, H - line), fill=body, outline=rim, width=line)
        if ps:
            color = {"a": (124, 156, 255, 255), "b": (255, 102, 102, 255), "x": (240, 140, 210, 255),
                     "y": (64, 214, 160, 255)}[button]
            c, r, w = W / 2, H * 0.22, int(H * 0.08)
            if button == "a":
                d.line((c - r, c - r, c + r, c + r), fill=color, width=w)
                d.line((c - r, c + r, c + r, c - r), fill=color, width=w)
            elif button == "b":
                d.ellipse((c - r, c - r, c + r, c + r), outline=color, width=w)
            elif button == "x":
                d.rectangle((c - r * 0.85, c - r * 0.85, c + r * 0.85, c + r * 0.85), outline=color, width=w)
            else:
                d.polygon([(c, c - r * 1.05), (c - r * 1.1, c + r * 0.75), (c + r * 1.1, c + r * 0.75)],
                          outline=color, width=w)
        else:
            color = {"a": (108, 194, 74, 255), "b": (229, 71, 59, 255), "x": (63, 142, 230, 255),
                     "y": (244, 196, 48, 255)}[button]
            text(d, W, button.upper(), color, 0.6)
        return finish(img)
    if button in ("leftshoulder", "rightshoulder"):
        img, W = canvas(1.55)
        d = ImageDraw.Draw(img)
        d.rounded_rectangle((line, H * 0.14, W - line, H * 0.86), radius=H * 0.3, fill=body, outline=rim, width=line)
        text(d, W, PAD_NAMES[button][1 if ps else 0], ink, 0.46)
        return finish(img)
    if button in ("lefttrigger", "righttrigger"):
        img, W = canvas(1.25)
        d = ImageDraw.Draw(img)
        d.rounded_rectangle((line, line, W - line, H - line), radius=H * 0.32, fill=body, outline=rim, width=line)
        d.rectangle((line * 2, H * 0.55, W - line * 2, H - line * 2), fill=body)
        text(d, W, PAD_NAMES[button][1 if ps else 0], ink, 0.44)
        return finish(img)
    if button in ("leftstick", "rightstick", "lstick", "rstick"):
        img, W = canvas(1.0)
        d = ImageDraw.Draw(img)
        d.ellipse((line, line, W - line, H - line), fill=body, outline=rim, width=line)
        if button in ("lstick", "rstick"):
            d.ellipse((H * 0.24, H * 0.24, H * 0.76, H * 0.76), outline=rim, width=line)
            text(d, W, "L" if button == "lstick" else "R", ink, 0.38)
        else:
            text(d, W, PAD_NAMES[button][1 if ps else 0], ink, 0.4)
        return finish(img)
    if button.startswith("dp"):
        img, W = canvas(1.0)
        d = ImageDraw.Draw(img)
        a, b = H * 0.31, H * 0.69
        arms = {"dpup": (a, line, b, a + line), "dpdown": (a, b - line, b, H - line), "dpleft": (line, a, a + line, b),
                "dpright": (b - line, a, H - line, b)}
        d.rectangle((a, line, b, H - line), fill=body, outline=rim, width=line)
        d.rectangle((line, a, H - line, b), fill=body, outline=rim, width=line)
        d.rectangle((a + line, a, b - line, b), fill=body)
        d.rectangle((a, a + line, b, b - line), fill=body)
        x0, y0, x1, y1 = arms[button]
        d.rectangle((x0 + line, y0 + line, x1 - line, y1 - line), fill=(250, 200, 90, 255))
        return finish(img)
    if button in ("start", "back"):
        img, W = canvas(1.0)
        d = ImageDraw.Draw(img)
        d.ellipse((H * 0.1, H * 0.1, H * 0.9, H * 0.9), fill=body, outline=rim, width=line)
        c = H / 2
        if button == "start":  # three lines: Xbox Menu, PlayStation Options
            for k in (-1, 0, 1):
                y = c + k * H * 0.13
                d.line((c - H * 0.17, y, c + H * 0.17, y), fill=ink, width=line)
        elif ps:  # Share/Create: a small arrow up
            d.line((c, c - H * 0.18, c, c + H * 0.16), fill=ink, width=line)
            d.line((c - H * 0.12, c - H * 0.06, c, c - H * 0.18, c + H * 0.12, c - H * 0.06), fill=ink, width=line)
        else:  # View: two windows
            d.rectangle((c - H * 0.17, c - H * 0.15, c + H * 0.05, c + H * 0.05), outline=ink, width=line)
            d.rectangle((c - H * 0.05, c - H * 0.03, c + H * 0.17, c + H * 0.17), fill=ink)
        return finish(img)
    # Anything else (touchpad, paddles, misc): its name on a rounded key.
    label = PAD_NAMES.get(button, (button, button))[1 if ps else 0]
    img, W = canvas(max(1.0, 0.36 * len(label)))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle((line, H * 0.14, W - line, H * 0.86), radius=H * 0.25, fill=body, outline=rim, width=line)
    text(d, W, label, ink, 0.4)
    return finish(img)


class Gamepads:
    """Controllers through SDL3, the game's own library (gow3_pc/out in a release, MSYS2 here):
    which buttons are held, and whether the first one is a PlayStation controller."""
    PLAYSTATION_TYPES = (4, 5, 6)  # SDL_GAMEPAD_TYPE_PS3, PS4, PS5

    def __init__(self):
        self.sdl = None
        self.pads = {}
        msys = Path(os.environ.get("MSYS2_ROOT", "C:/msys64"))
        for path in (ROOT / "gow3_pc/out/SDL3.dll", msys / "clang64/bin/SDL3.dll", ROOT.parent / "msys64/clang64/bin/SDL3.dll"):
            if not path.is_file():
                continue
            try:
                sdl = ctypes.CDLL(str(path))
            except OSError:
                continue
            sdl.SDL_SetHint.argtypes = [ctypes.c_char_p, ctypes.c_char_p]
            sdl.SDL_SetHint.restype = ctypes.c_bool
            sdl.SDL_Init.argtypes = [ctypes.c_uint32]
            sdl.SDL_Init.restype = ctypes.c_bool
            sdl.SDL_GetGamepads.argtypes = [ctypes.POINTER(ctypes.c_int)]
            sdl.SDL_GetGamepads.restype = ctypes.POINTER(ctypes.c_uint32)
            sdl.SDL_OpenGamepad.argtypes = [ctypes.c_uint32]
            sdl.SDL_OpenGamepad.restype = ctypes.c_void_p
            sdl.SDL_CloseGamepad.argtypes = [ctypes.c_void_p]
            sdl.SDL_GetGamepadButton.argtypes = [ctypes.c_void_p, ctypes.c_int]
            sdl.SDL_GetGamepadButton.restype = ctypes.c_bool
            sdl.SDL_GetGamepadAxis.argtypes = [ctypes.c_void_p, ctypes.c_int]
            sdl.SDL_GetGamepadAxis.restype = ctypes.c_int16
            sdl.SDL_GetGamepadType.argtypes = [ctypes.c_void_p]
            sdl.SDL_GetGamepadType.restype = ctypes.c_int
            sdl.SDL_free.argtypes = [ctypes.c_void_p]
            # The launcher has no SDL window: controller input arrives anyway.
            sdl.SDL_SetHint(b"SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", b"1")
            if sdl.SDL_Init(0x2000):  # SDL_INIT_GAMEPAD
                self.sdl = sdl
            break

    def refresh(self):
        if not self.sdl:
            return []
        self.sdl.SDL_PumpEvents()
        count = ctypes.c_int(0)
        ids = self.sdl.SDL_GetGamepads(ctypes.byref(count))
        current = [ids[i] for i in range(count.value)] if ids else []
        if ids:
            self.sdl.SDL_free(ctypes.cast(ids, ctypes.c_void_p))
        for jid in current:
            if jid not in self.pads:
                pad = self.sdl.SDL_OpenGamepad(jid)
                if pad:
                    self.pads[jid] = pad
        for jid in [j for j in self.pads if j not in current]:
            self.sdl.SDL_CloseGamepad(self.pads.pop(jid))
        self.sdl.SDL_UpdateGamepads()
        return list(self.pads.values())

    def held(self):
        down = set()
        for pad in self.refresh():
            for index, name in enumerate(SDL_BUTTONS):
                if self.sdl.SDL_GetGamepadButton(pad, index):
                    down.add(name)
            for axis, name in ((4, "lefttrigger"), (5, "righttrigger")):
                if self.sdl.SDL_GetGamepadAxis(pad, axis) > 16000:
                    down.add(name)
        return down

    def style(self):
        for pad in self.refresh():
            return "playstation" if self.sdl.SDL_GetGamepadType(pad) in self.PLAYSTATION_TYPES else "xbox"
        return None

    def axes(self):
        """The first pad's left stick, -1..1 (up and left are negative). The pads are those of the
        last refresh(), i.e. of held(); no deadzone here."""
        for pad in self.pads.values():
            return {"lx": max(-1.0, self.sdl.SDL_GetGamepadAxis(pad, 0) / 32767),
                    "ly": max(-1.0, self.sdl.SDL_GetGamepadAxis(pad, 1) / 32767)}
        return {"lx": 0.0, "ly": 0.0}


class PadNav:
    """24 Oct 2026: the launcher with a controller (Steam Deck, couch). D-pad or left stick move the
    focus, A presses, B goes back, LB/RB change the section, Start plays. Only while the launcher
    window has the focus (SDL reads pads in the background) and no game runs (the game owns the
    pad then). Buttons still held when the nav becomes active (Start as the game exits) are
    ignored until released."""
    DIRECTIONS = {"dpup": "up", "dpdown": "down", "dpleft": "left", "dpright": "right"}
    STICK = 0.5
    FIRST_REPEAT, REPEAT = 0.4, 0.12

    def __init__(self, launcher):
        self.launcher, self.root = launcher, launcher.root
        self.armed, self.ignored, self.seen, self.due = False, set(), set(), {}
        self.job = None

    @property
    def gamepads(self):
        if not hasattr(self.launcher, "gamepads"):  # page_controls makes it too; one object
            self.launcher.gamepads = Gamepads()
        return self.launcher.gamepads

    def start(self):
        self.job = self.root.after(33, self.tick)

    def stop(self):
        if self.job is not None:
            self.root.after_cancel(self.job)
            self.job = None

    def tick(self):
        try:
            self.job = self.root.after(33, self.tick)  # first: a dialog opened below keeps ticking
            self.step(time.monotonic())
        except tk.TclError:  # the root is gone
            self.job = None

    def window_focused(self):
        return self.root.focus_displayof() is not None and bool(self.root.winfo_viewable())

    def active(self):
        launcher = self.launcher
        return (launcher.process is None and getattr(launcher, "capturing", None) is None
                and getattr(launcher, "pad_capture", None) is None and self.window_focused())

    def step(self, now):
        if not self.active():
            self.armed = False
            return
        pads = self.gamepads  # made here, on first use
        if not pads.sdl:
            self.armed = False
            return self.stop()  # no SDL3.dll: nothing to poll, ever
        down = {self.DIRECTIONS.get(name, name) for name in pads.held()}
        axes = pads.axes()
        for name, hit in (("up", axes["ly"] < -self.STICK), ("down", axes["ly"] > self.STICK),
                          ("left", axes["lx"] < -self.STICK), ("right", axes["lx"] > self.STICK)):
            if hit:
                down.add(name)
        if not self.armed:
            self.armed, self.ignored = True, set(down)
        self.ignored &= down
        live = down - self.ignored
        fire = []
        for name in sorted(live):
            if name not in self.seen:
                fire.append(name)
                if name in self.DIRECTIONS.values():
                    self.due[name] = now + self.FIRST_REPEAT
            elif name in self.due and now >= self.due[name]:
                fire.append(name)
                self.due[name] = now + self.REPEAT
        self.seen = live
        self.due = {name: when for name, when in self.due.items() if name in live}
        for name in fire:  # last: a press can open a dialog (a nested loop)
            self.press(name)

    def press(self, name):
        launcher = self.launcher
        dialog = Dialog.current
        if dialog is not None:
            if name == "a":
                dialog.press_default()
            elif name == "b":
                dialog.press_cancel()
            elif name in ("left", "right"):
                dialog.step_button(-1 if name == "left" else 1)
            return
        if name in ("leftshoulder", "rightshoulder"):
            return launcher.step_section(-1 if name == "leftshoulder" else 1)
        if name == "start":
            return launcher.play_or_stop()
        try:
            focus = self.root.focus_get()
        except (KeyError, tk.TclError):  # a menu's popup
            return
        if focus is None or focus.winfo_toplevel() is not self.root:
            if name in ("up", "down", "a"):
                launcher.play_button.focus_set()
            return
        if name == "a":
            if not isinstance(focus, Dropdown):  # its popup menu cannot be driven by the pad (left/right change it)
                focus.event_generate("<Return>")  # the same path as the keyboard
        elif name == "b":
            target = launcher.tab_buttons[launcher.section][0]
            if focus is target:
                target = launcher.cards[launcher.config["game"]][0]
            target.focus_set()
        elif name in ("up", "down", "left", "right"):
            horizontal = name in ("left", "right")
            step = -1 if name in ("up", "left") else 1
            if horizontal and isinstance(focus, Segmented):
                focus.move(step)
            elif horizontal and isinstance(focus, Toggle):
                if focus.enabled and focus.value != (step > 0):
                    focus.set(step > 0, notify=True)
            elif horizontal and isinstance(focus, Dropdown):
                focus.move(step)  # its popup menu is native: the pad steps through the options instead
            else:
                target = focus.tk_focusNext() if step > 0 else focus.tk_focusPrev()
                if target is not None:
                    target.focus_set()


STICK_ACTIONS = {"ls_up", "ls_down", "ls_left", "ls_right", "rs_up", "rs_down", "rs_left", "rs_right"}
DEFAULT_CONTROLS = {
    "ls_up": ["key:W"], "ls_down": ["key:S"], "ls_left": ["key:A"], "ls_right": ["key:D"],
    "rs_up": ["key:Up"], "rs_down": ["key:Down"], "rs_left": ["key:Left"], "rs_right": ["key:Right"],
    "left": ["key:1", "mouse:wheel_down"], "right": ["key:2", "mouse:wheel_up"], "up": ["key:3"],
    "down": ["key:4"],
    "cross": ["key:Space"], "circle": ["key:Left Ctrl"], "square": ["key:F"], "triangle": ["key:R"],
    "l2": ["mouse:right"], "r2": ["mouse:left"], "l1": ["key:Q", "mouse:x1"], "r1": ["key:E", "mouse:x2"],
    "l3": ["key:Left Shift"], "r3": ["key:V", "mouse:middle"], "options": ["key:Escape", "key:Return"],
    "touchpad_left": ["key:Tab"], "touchpad_right": ["key:Backspace"],
}
# Button icons in the games' text and menus (runtime GOW3_PROMPTS): the device used last, or fixed.
PROMPT_MODES = [("auto", "Auto"), ("keyboard", "Keyboard and mouse"), ("playstation", "PlayStation"),
                ("xbox", "Xbox")]
MOUSE_SENSITIVITIES = [("0.5", "0.5x"), ("0.75", "0.75x"), ("1.0", "1x"), ("1.5", "1.5x"), ("2.0", "2x"),
                       ("3.0", "3x"), ("4.0", "4x"), ("5.0", "5x")]
# While aiming (times the sensitivity) and up/down compared with left/right (the in-game menu's
# Controls tab has the same as sliders).
MOUSE_FACTORS = [("0.5", "0.5x"), ("0.75", "0.75x"), ("1.0", "1x"), ("1.25", "1.25x"), ("1.5", "1.5x"),
                 ("2.0", "2x")]
MOUSE_AIM_FACTORS = [("0.5", "0.5x"), ("1.0", "1x"), ("2.0", "2x"), ("3.0", "3x"), ("5.0", "5x"),
                     ("10.0", "10x")]
MOUSE_LABELS = {"left": "Left mouse", "middle": "Middle mouse", "right": "Right mouse", "x1": "Mouse back",
                "x2": "Mouse forward", "wheel_up": "Wheel up", "wheel_down": "Wheel down"}
# Tk key names -> SDL key names (letters and digits map to themselves, upper case).
TK_TO_SDL = {
    "space": "Space", "Return": "Return", "Escape": "Escape", "Tab": "Tab", "BackSpace": "Backspace",
    "Shift_L": "Left Shift", "Shift_R": "Right Shift", "Control_L": "Left Ctrl", "Control_R": "Right Ctrl",
    "Alt_L": "Left Alt", "Alt_R": "Right Alt", "Caps_Lock": "CapsLock", "Up": "Up", "Down": "Down",
    "Left": "Left", "Right": "Right", "Delete": "Delete", "Home": "Home", "End": "End", "Prior": "PageUp",
    "Next": "PageDown", "minus": "-", "equal": "=", "bracketleft": "[", "bracketright": "]",
    "backslash": "\\", "semicolon": ";", "apostrophe": "'", "grave": "`", "period": ".", "slash": "/",
    "KP_Divide": "Keypad /", "KP_Multiply": "Keypad *", "KP_Subtract": "Keypad -", "KP_Add": "Keypad +",
    "KP_Enter": "Keypad Enter", "KP_Decimal": "Keypad .", "Pause": "Pause", "Scroll_Lock": "ScrollLock",
}
TK_TO_SDL.update({f"F{n}": f"F{n}" for n in range(1, 13)})
TK_TO_SDL.update({f"KP_{n}": f"Keypad {n}" for n in range(10)})
# Keys the runtime keeps for itself: Insert opens the in-game menu, F9 records the pad.
RESERVED_KEYS = {"Insert", "F9", "comma"}


def merged_controls(controls):
    merged = {action: list(bindings) for action, bindings in DEFAULT_CONTROLS.items()}
    for action, bindings in (controls or {}).items():
        if action in merged:
            merged[action] = list(bindings)[:2]
    return merged


def control_maps(controls):
    """GOW3_KEY_MAP and GOW3_MOUSE_MAP for a game's controls (every action listed, so unbound ones
    lose the runtime's defaults)."""
    keys, mouse = [], []
    for action, bindings in merged_controls(controls).items():
        names = [b[4:] for b in bindings if b.startswith("key:")]
        keys += [f"{action}={name}" for name in names] or [f"{action}="]
        if action not in STICK_ACTIONS:
            buttons = [b[6:] for b in bindings if b.startswith("mouse:")]
            mouse += [f"{action}={name}" for name in buttons] or [f"{action}="]
    return ",".join(keys), ",".join(mouse)


def parse_map(spec):
    """runtime_pad.c's map syntax: (action, source name) per entry ("" = none)."""
    entries = []
    for entry in (spec or "").split(","):
        action, eq, name = entry.partition("=")
        if eq:
            entries.append((action.strip(), name.strip()))
    return entries


def apply_map(bindings, spec, prefix):
    """A map on top of an action -> ["key:..", "mouse:.."] dict, as the runtime applies it: an
    action's first entry replaces its sources of this kind, further ones add, "action=" none."""
    replaced = set()
    for action, name in parse_map(spec):
        if action not in bindings:
            continue
        if action not in replaced:
            replaced.add(action)
            bindings[action] = [b for b in bindings[action] if not b.startswith(prefix)]
        if name:
            bindings[action].append(prefix + name)


def sensitivity_value(text):
    """A mouse sensitivity as the Sensitivity choice names it (the in-game menu sets any)."""
    try:
        value = float(text)
    except (TypeError, ValueError):
        return "1.0"
    for option, _ in MOUSE_SENSITIVITIES:
        if abs(float(option) - value) < 0.005:
            return option
    return f"{value:g}"


def factor_value(text, options):
    """A mouse factor from a settings file as one of the options when it is one."""
    try:
        value = float(text)
    except ValueError:
        return "1.0"
    for option, _ in options:
        if abs(float(option) - value) < 0.005:
            return option
    return f"{value:g}"


# The controls in each game's settings file: the game reads them there, and its in-game menu
# (Insert / R3+L2, Controls tab) changes them too.
def controls_to_ini(settings):
    keys, mouse = control_maps(settings.get("controls"))
    return {"key_map": keys, "mouse_map": mouse, "pad_map": pad_map(settings.get("pad_controls")),
            "mouse": "1" if settings.get("mouse", True) else "0",
            "mouse_sens": settings.get("mouse_sens", "1.0"),
            "mouse_invert": "1" if settings.get("mouse_invert") else "0",
            "prompts": settings.get("prompts", "auto"),
            "mouse_aim_sens": settings.get("mouse_aim_sens", "1.0"),
            "mouse_vertical": settings.get("mouse_vertical", "1.0"),
            "mouse_invert_x": "1" if settings.get("mouse_invert_x") else "0",
            "vibration": "1" if settings.get("vibration", True) else "0"}


def controls_from_ini(values, settings):
    """A game's controls from its settings file into the launcher's settings; False when the file
    has none yet."""
    if not any(key in values for key in ("key_map", "mouse_map", "pad_map")):
        return False
    controls = {action: list(bindings) for action, bindings in DEFAULT_CONTROLS.items()}
    apply_map(controls, values.get("key_map"), "key:")
    apply_map(controls, values.get("mouse_map"), "mouse:")
    settings["controls"] = {action: bindings[:2] for action, bindings in controls.items()}
    pad = dict(PAD_DEFAULTS)
    replaced = set()
    for action, name in parse_map(values.get("pad_map")):
        if action in pad and action not in replaced:
            replaced.add(action)
            pad[action] = name or None
    settings["pad_controls"] = {action: button for action, button in pad.items() if button != PAD_DEFAULTS.get(action)}
    if "mouse" in values:
        settings["mouse"] = values["mouse"] != "0"
    if "mouse_sens" in values:
        settings["mouse_sens"] = sensitivity_value(values["mouse_sens"])
    if "mouse_invert" in values:
        settings["mouse_invert"] = values["mouse_invert"] == "1"
    for key, options in (("mouse_aim_sens", MOUSE_AIM_FACTORS), ("mouse_vertical", MOUSE_FACTORS)):
        if key in values:
            settings[key] = factor_value(values[key], options)
    if "mouse_invert_x" in values:
        settings["mouse_invert_x"] = values["mouse_invert_x"] == "1"
    if "vibration" in values:
        settings["vibration"] = values["vibration"] == "1"
    if "prompts" in values:
        settings["prompts"] = values["prompts"] if values["prompts"] in dict(PROMPT_MODES) else "auto"
    return True


def binding_label(binding):
    if not binding:
        return "—"
    kind, _, name = binding.partition(":")
    return MOUSE_LABELS.get(name, name) if kind == "mouse" else name


def is_game_dir(folder):
    """The Collection's game folder (CUSA02320, CUSA02344): eboot.bin and sce_sys/param.sfo."""
    return bool(folder) and (Path(folder) / "eboot.bin").is_file() and \
        (Path(folder) / "sce_sys" / "param.sfo").is_file()


UPDATE_SUFFIXES = ("-patch", "-update", "_patch", "_update")


def game_update_dir(folder):
    """The game's update (e.g. 1.02) next to its folder, found as tools/probe_uncharted.py does:
    named like the game folder plus -patch or -update, with or without uroot."""
    folder = Path(folder)
    title = folder.parent if folder.name.lower() == "uroot" else folder
    names = {title.name.lower() + suffix for suffix in UPDATE_SUFFIXES}
    try:
        for entry in title.parent.iterdir():
            if entry.name.lower() in names:
                for candidate in (entry / "uroot", entry):
                    if is_game_dir(candidate):
                        return candidate
    except OSError:
        pass
    return None


def app_version(folder):
    """APP_VER of a game folder (sce_sys/param.sfo); 01.02 when the executables are 1.02 although
    param.sfo says otherwise (dumps with the update merged in that kept the 1.00 param.sfo)."""
    try:
        data = (Path(folder) / "sce_sys" / "param.sfo").read_bytes()
        _, _, keys, values, count = struct.unpack_from("<IIIII", data)
        for i in range(count):
            key_at, _, length, _, value_at = struct.unpack_from("<HHIII", data, 20 + i * 16)
            key = data[keys + key_at:data.index(bytes(1), keys + key_at)].decode()
            if key == "APP_VER":
                version = data[values + value_at:values + value_at + length].rstrip(bytes(1)).decode()
                return "01.02" if version < "01.02" and has_update_102_code(folder) else version
    except (OSError, ValueError, struct.error):
        pass
    return None


# Uncharted 2's torch code as update 1.02 compiled it (tools/probe_uncharted.py
# TORCH_FORWARD_PATCHES; 1.00 lays this code out differently).
# 20 Oct 2026 audit: twin of TORCH_FORWARD_PATCHES[0][1][0][0] in tools/probe_uncharted.py (copied, not
# imported: that module is heavy at start); app_version below is the twin of sfo() in gow3_pc/scripts/prepare.py.
UPDATE_102_CODE = bytes.fromhex("488d05cc9fd500 8b4850 84c9 7440 8a50".replace(" ", ""))
_update_102_cache = {}


def has_update_102_code(folder):
    path = Path(folder) / "big2-ps4_Shipping.elf"
    try:
        key = (str(path), path.stat().st_mtime_ns)
    except OSError:
        return False
    if key not in _update_102_cache:
        try:
            _update_102_cache[key] = UPDATE_102_CODE in path.read_bytes()
        except OSError:
            _update_102_cache[key] = False
    return _update_102_cache[key]


def desktop_folder():
    """The user's Desktop folder. Windows can move it (OneDrive backup, a redirected or renamed
    Desktop), so it is asked for (SHGetKnownFolderPath) instead of assumed at %USERPROFILE%\\Desktop:
    shortcuts failed there with "DirectoryNotFound" (#39)."""
    try:
        from ctypes import wintypes

        class GUID(ctypes.Structure):
            _fields_ = [("Data1", wintypes.DWORD), ("Data2", wintypes.WORD), ("Data3", wintypes.WORD),
                        ("Data4", ctypes.c_ubyte * 8)]

        desktop_id = GUID(0xB4BFCC3A, 0xDB2C, 0x424C,  # FOLDERID_Desktop
                          (ctypes.c_ubyte * 8)(0xB0, 0x29, 0x7F, 0xE9, 0x9A, 0x87, 0xC6, 0x41))
        found = ctypes.c_void_p()
        shell32 = ctypes.windll.shell32
        shell32.SHGetKnownFolderPath.argtypes = [ctypes.POINTER(GUID), wintypes.DWORD, wintypes.HANDLE,
                                                 ctypes.POINTER(ctypes.c_void_p)]
        if shell32.SHGetKnownFolderPath(ctypes.byref(desktop_id), 0, None, ctypes.byref(found)) == 0:
            try:
                folder = Path(ctypes.wstring_at(found.value))
            finally:
                ctypes.windll.ole32.CoTaskMemFree(found)
            if folder.is_dir():
                return folder
    except (AttributeError, OSError, ValueError):
        pass
    return Path(os.environ.get("USERPROFILE", Path.home())) / "Desktop"


def monitor_refresh_hz():
    try:
        user32, gdi32 = ctypes.windll.user32, ctypes.windll.gdi32
        dc = user32.GetDC(0)
        hz = gdi32.GetDeviceCaps(dc, 116)  # VREFRESH
        user32.ReleaseDC(0, dc)
        return hz if hz > 1 else 60
    except Exception:
        return 60


def fps_limit_value(value):
    """A listed limit; unlimited (0) and faster ones are 120 (144-240 are listed only with Frame Generation)."""
    listed = {str(v) for v, _ in fps_limits()}
    if value in listed:
        return value
    try:
        number = int(value)
    except (TypeError, ValueError):
        return "60"
    return str(MAX_FPS) if number == 0 or number > MAX_FPS else "60"


RESOLUTIONS = [("960x540", "540p"), ("1280x720", "720p"), ("1600x900", "900p"), ("1920x1080", "1080p"), ("2560x1440", "1440p"),
               ("2880x1620", "1620p"), ("3840x2160", "4K")]
# Ultrawide: the render resolution's height at a wider aspect ratio. The games widen their view
# (more to the sides, nothing stretched); menus and the HUD keep their layout. 16:10 (Steam Deck,
# 1280x800): the render resolution's width, taller; the games keep the 16:9 width of view and
# show more at the top and bottom.
ASPECTS = [("16:10", "16:10"), ("16:9", "16:9"), ("21:9", "21:9"), ("32:9", "32:9")]
ASPECT_WIDTHS = {"16:10": 16 / 10, "16:9": 16 / 9, "21:9": 64 / 27, "32:9": 32 / 9}


def aspect_resolution(resolution, aspect):
    """The render resolution at the aspect ratio: 1440p at 21:9 is 3440x1440 (widths in steps of 8),
    720p at 16:10 is 1280x800 (heights in steps of 4)."""
    width, height = (int(v) for v in resolution.split("x"))
    if aspect not in ASPECT_WIDTHS or aspect == "16:9":
        return resolution
    if ASPECT_WIDTHS[aspect] < 16 / 9:
        height = int(round(width / ASPECT_WIDTHS[aspect] / 4)) * 4
    elif aspect == "21:9" and height in (1080, 1440, 1600):
        width = {1080: 2560, 1440: 3440, 1600: 3840}[height]  # the common monitor sizes
    else:
        width = int(round(height * ASPECT_WIDTHS[aspect] / 8)) * 8
    return f"{width}x{height}"


def resolution_options(aspect):
    """RESOLUTIONS labelled by the height the game draws at this aspect ratio (720p is 800p at
    16:10; wider ones keep the height); 2160 lines are 4K, ultrawide included."""
    options = []
    for value, _ in RESOLUTIONS:
        height = int(aspect_resolution(value, aspect).split("x")[1])
        options.append((value, "4K" if height == 2160 else f"{height}p"))
    return options

# Quality presets: the game has one detail level (the PS4's); quality here is the resolution the
# game draws at, how it reaches the screen (FSR 1, sharpening) and the frame rate. Above 1080p the
# render resolution is new (October 2026): confirm in play. All three games draw at 4K
# natively except Drake's Fortune, which stops at 1620p (its memory layout) and upscales.
PRESETS = {
    "low": {
        "title": "LOW", "summary": "60 FPS  •  720p + FSR 1",
        "detail": "For integrated and older graphics cards: the game draws at 720p and FSR 1 scales "
                  "it up to your screen.",
        "resolution": "1280x720",
        "values": {"fps_limit": "60", "frames_queued": "1", "vsync": "1", "async_shaders": "1",
                   "fsr1": "1", "rcas": "1", "rcas_strength": "75"},
    },
    "medium": {
        "title": "MEDIUM", "summary": "60 FPS  •  as on PS4",
        "detail": "Like the PS4: 1080p at 60 FPS. Right for most PCs.",
        "resolution": "1920x1080",
        "values": {"fps_limit": "60", "frames_queued": "1", "vsync": "1", "async_shaders": "1",
                   "fsr1": "0", "rcas": "1", "rcas_strength": "75"},
    },
    "high": {
        "title": "HIGH", "summary": "60 FPS  •  1440p",
        "detail": "Sharper: the game draws at 1440p. On a 4K screen FSR 1 scales it up.",
        "resolution": "2560x1440",
        "values": {"fps_limit": "60", "frames_queued": "1", "vsync": "1", "async_shaders": "1",
                   "fsr1": None, "rcas": "1", "rcas_strength": "50"},
    },
    "ultra": {
        "title": "ULTRA", "summary": "4K  •  up to 120 FPS",
        "detail": "For fast graphics cards: native 4K and up to 120 FPS (experimental). Drake's "
                  "Fortune draws at 1620p and scales up.",
        "resolution": "3840x2160",
        "values": {"fps_limit": str(MAX_FPS), "frames_queued": "1", "vsync": "1", "async_shaders": "1",
                   "fsr1": None, "rcas": "1", "rcas_strength": "50"},
    },
}
PRESET_ORDER = ("low", "medium", "high", "ultra")


def anti_aliasing_options():
    """Temporal anti-aliasing and upscaling (the runtime's "upscaler" setting): FSR 4 when its model
    files are installed, DLSS when the DLSS files are beside Uncharted.exe."""
    options = [("off", "Off"), ("taa", "TAA"), ("fsr3", "FSR 3.1")]
    if (ROOT / "gow3_pc/fsr4_shaders/fsr4_model_v07_i8_native_1080_pre.spv").is_file():
        options.append(("fsr4", "FSR 4"))
    out = ROOT / "gow3_pc/out"
    if (out / "gow3_dlss.dll").is_file() and (out / "nvngx_dlss.dll").is_file():
        options.append(("dlss", "DLSS"))
    return options


def screen_size():
    try:
        user32 = ctypes.windll.user32
        return user32.GetSystemMetrics(0), user32.GetSystemMetrics(1)
    except Exception:
        return 1920, 1080


def work_area(root):
    """The primary screen without the taskbar (x, y, width, height), in Tk's pixels."""
    try:
        rect = (ctypes.c_long * 4)()
        if ctypes.windll.user32.SystemParametersInfoW(0x30, 0, rect, 0):  # SPI_GETWORKAREA
            return rect[0], rect[1], rect[2] - rect[0], rect[3] - rect[1]
    except (AttributeError, OSError):
        pass
    return 0, 0, root.winfo_screenwidth(), root.winfo_screenheight()


def fsr1_for(resolution):
    """FSR 1 when the game's image is smaller than the screen (it upscales; else no effect)."""
    width, height = (int(v) for v in resolution.split("x"))
    screen_w, screen_h = screen_size()
    return "1" if width < screen_w and height < screen_h else "0"


def preset_values(name):
    values = dict(PRESETS[name]["values"])
    if values["fsr1"] is None:
        values["fsr1"] = fsr1_for(PRESETS[name]["resolution"])
    return values


def gpu_names():
    """The graphics cards: the one the game used last (its log names it), else Windows' list."""
    logs = [ROOT / "analysis" / g["log"] / "boot.log" for g in GAMES]
    for log in sorted((log for log in logs if log.exists()), key=lambda p: p.stat().st_mtime, reverse=True):
        try:
            with open(log, encoding="utf-8", errors="replace") as lines:
                for number, line in enumerate(lines):
                    if line.startswith("GPU: using "):
                        return [line[len("GPU: using "):].strip()]
                    if number > 400:
                        break
        except OSError:
            pass
    try:
        result = subprocess.run(["powershell", "-NoProfile", "-Command", "(Get-CimInstance Win32_VideoController).Name"],
                                capture_output=True, text=True, timeout=20, creationflags=NO_WINDOW)
        return [line.strip() for line in result.stdout.splitlines() if line.strip()]
    except (OSError, subprocess.SubprocessError):
        return []


_VULKAN_GPUS = None


def vulkan_gpus():
    """The Vulkan devices as (index, name), in the order GOW3_GPU_ID counts them; [] when the
    tool is missing or fails. Read once (creating a Vulkan instance takes a moment)."""
    global _VULKAN_GPUS
    if _VULKAN_GPUS is None:
        _VULKAN_GPUS = []
        tool = ROOT / "gow3_pc" / "out" / "gow3-gpu-capabilities.exe"
        try:
            result = subprocess.run([str(tool), "--list"], capture_output=True, text=True,
                                    timeout=10, creationflags=NO_WINDOW)
            for line in result.stdout.splitlines():
                parts = line.split("\t")
                if len(parts) >= 2 and parts[0].strip().isdigit():
                    _VULKAN_GPUS.append((parts[0].strip(), parts[1].strip()))
        except (OSError, subprocess.SubprocessError):
            pass
    return _VULKAN_GPUS


def gpu_tier(name):
    """The Home preset for a graphics card, from its model name; None when not known.
    Rough classes: Low = integrated and older entry cards, Medium = 1080p cards (GTX 1060-1660,
    RTX x060, RX 580-7600), High = 1440p cards, Ultra = 4K cards (RTX x080/x090,
    RTX 4070 Ti and up, RX 6800 XT/7900/9070). High and Ultra laptop chips count one class lower."""
    n = (name or "").lower().replace("(tm)", "").replace("(r)", "")
    nvidia = re.search(r"\b(rtx|gtx|gt|mx)\s*(\d{3,4})\s*(ti)?\s*(super)?", n)
    amd = re.search(r"\brx\s*(\d{3,4})\s*(xtx|xt|gre)?", n)
    arc = re.search(r"\barc\s*([ab])(\d{3})", n)
    if nvidia:
        kind, number = nvidia.group(1), int(nvidia.group(2))
        ti, super_ = bool(nvidia.group(3)), bool(nvidia.group(4))
        if kind in ("gt", "mx"):
            tier = "low"
        elif kind == "gtx":
            if number == 1080 and ti:
                tier = "high"
            elif 1060 <= number < 1100 or 1660 <= number < 1700 or (number == 1650 and super_):
                tier = "medium"
            else:
                tier = "low"
        else:
            series, model = number // 100, number % 100
            if model >= 80:
                tier = "ultra" if series >= 30 else "high"
            elif model == 70:
                tier = "ultra" if series >= 40 and (ti or super_) else "high"
            elif model == 60:
                tier = "high" if ti or series >= 50 else "medium"
            else:
                tier = "medium"
    elif amd:
        number, suffix = int(amd.group(1)), amd.group(2) or ""
        if number >= 9000:
            tier = {7: "ultra", 6: "high"}.get((number % 100) // 10, "medium")
        elif number >= 1000:
            series, model = number // 1000, (number % 1000) // 100
            if model >= 9:
                tier = "ultra"
            elif model == 8:
                tier = "ultra" if suffix.startswith("xt") else "high"
            elif model == 7:
                tier = "medium" if series == 5 else "high"
            elif model == 6 or number == 5500:
                tier = "medium"
            else:
                tier = "low"
        else:
            tier = "medium" if number in (470, 480, 570, 580, 590) else "low"
    elif "vega 56" in n or "vega 64" in n:
        tier = "medium"
    elif "radeon vii" in n:
        tier = "high"
    elif arc:
        tier = "medium" if int(arc.group(2)) >= 500 else "low"
    elif "intel" in n or ("radeon" in n and "graphics" in n):
        tier = "low"  # integrated graphics
    else:
        return None
    if "laptop" in n or "max-q" in n or "mobile" in n or re.search(r"\d{3,4}m\b", n):
        tier = {"ultra": "high", "high": "medium"}.get(tier, tier)
    return tier


# ---------------------------------------------------------------------------------------------
# Settings files

def ini_file(game):
    """Each game keeps its own runtime settings (tools/probe_uncharted.py passes the same file)."""
    return USER_DIR / f"probe-{game}.ini"


def ini_lines(game):
    """The game's settings file, or the shared one of earlier versions as its starting point."""
    # 20 Oct 2026 audit: fall back to the shared file only when the game's own is missing (as
    # tools/probe_uncharted.py does). Any OSError used to pick it, and write_ini then wrote the
    # shared file's contents over the game's ini while another process held it.
    path = ini_file(game)
    if path.exists():
        try:
            return path.read_text(encoding="utf-8", errors="replace").splitlines()
        except OSError:
            time.sleep(0.2)  # one retry for a transient lock; a second failure propagates
            return path.read_text(encoding="utf-8", errors="replace").splitlines()
    try:
        return INI_FILE.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError:
        return ["# Uncharted settings (launcher and in-game menu: Insert / R3+L2)"]


def read_ini(game):
    values = dict(INI_DEFAULTS)
    try:
        lines = ini_lines(game)
    except OSError as error:  # 20 Oct 2026 audit: a locked ini must not raise into the launcher's start or Tk callbacks
        print(f"Settings not read: {ini_file(game)} could not be read ({error}); defaults apply")
        lines = []
    for line in lines:
        if "=" in line and not line.lstrip().startswith("#"):
            key, value = line.split("=", 1)
            values[key.strip()] = value.strip()
    values["fps_limit"] = fps_limit_value(values["fps_limit"])
    return values


def ini_mtime(game):
    try:
        return ini_file(game).stat().st_mtime_ns
    except OSError:
        return None


def write_ini(game, updates):
    """Updates keys in place and keeps every other line (the in-game menu writes more keys)."""
    try:
        lines = ini_lines(game)
    except OSError as error:  # 20 Oct 2026 audit: writing now would replace the game's ini with partial data
        print(f"Settings not saved: {ini_file(game)} could not be read ({error})")
        return
    done = set()
    for i, line in enumerate(lines):
        if "=" in line and not line.lstrip().startswith("#"):
            key = line.split("=", 1)[0].strip()
            if key in updates:
                lines[i] = f"{key}={updates[key]}"
                done.add(key)
    lines += [f"{key}={value}" for key, value in updates.items() if key not in done]
    path = ini_file(game)
    path.parent.mkdir(parents=True, exist_ok=True)
    write_text_safely(path, "\n".join(lines) + "\n")


def load_config_file(path):
    """A launcher.json as a dict; {} when missing. One that can't be read (cut off while written)
    is kept beside it as .bad, and the other copy or the defaults apply."""
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
        return data if isinstance(data, dict) else {}
    except OSError:
        return {}
    except ValueError:
        try:
            shutil.copy2(path, path.with_name(path.name + ".bad"))
        except OSError:
            pass
        return {}


def read_config():
    config = dict(LAUNCHER_DEFAULTS)
    stored = load_config_file(CONFIG_FILE)
    if CONFIG_FILE != PREFIX_CONFIG_FILE:
        # Under Wine the shared file wins; what it lacks comes from this prefix's own file (the place
        # earlier versions used), game by game. That file is left as it is.
        older = load_config_file(PREFIX_CONFIG_FILE)
        if older:
            games = {}
            for source in (older, stored):
                for game, entry in (source.get("games") if isinstance(source.get("games"), dict) else {}).items():
                    if isinstance(entry, dict):
                        games.setdefault(game, {}).update(entry)
            stored = {**older, **stored, "games": games}
    config.update(stored)
    # Version 2: render resolutions above 1080p crash in gameplay; earlier presets chose them.
    if config.get("version", 1) < 2:
        config["resolution"] = "1920x1080"
    # Version 3: preset, resolution and multithreading per game; the shared values start all three.
    if not isinstance(config.get("games"), dict):
        config["games"] = {}
    games = config["games"]
    resolutions = [r for r, _ in RESOLUTIONS]
    for game in GAME_KEYS:
        if not isinstance(games.get(game), dict):
            games[game] = {}
        entry = games[game]
        for name, default in GAME_SETTING_DEFAULTS.items():
            entry.setdefault(name, config.get(name, default))
        entry["preset"] = {"balanced": "medium"}.get(entry["preset"], entry["preset"])
        # A value that isn't valid (another version, a hand edit) falls back alone.
        if entry["resolution"] not in resolutions:
            entry["resolution"] = "1920x1080"
        if entry.get("aspect") not in ASPECT_WIDTHS:
            entry["aspect"] = "16:9"
    # Version 4: the presets draw at 720p / 1080p / 1440p / 4K (all were 1080p before).
    if config.get("version", 1) < 4:
        for game, entry in games.items():
            if entry["preset"] in PRESETS:
                entry["resolution"] = PRESETS[entry["preset"]]["resolution"]
                write_ini(game, preset_values(entry["preset"]))
    # Version 5: every preset queues 2 frames (with 1 the CPU waits for the GPU each frame: heavy
    # Uncharted 2 scenes ran at 45 instead of 57 FPS). Not with multithreaded rendering, where
    # Uncharted 2 needs 1.
    if config.get("version", 1) < 5:
        for game, entry in games.items():
            if entry["preset"] in PRESETS and not entry.get("multithreaded"):
                write_ini(game, {"frames_queued": "2"})
    # Version 6: every preset queues 1 frame again: with 2, GPU results the game reads came a frame
    # later and Uncharted 2's Nate and NPCs stood above the floor (16 Oct). Custom settings stay.
    if config.get("version", 1) < 6:
        for game, entry in games.items():
            if entry["preset"] in PRESETS:
                write_ini(game, {"frames_queued": "1"})
    # Version 7: multithreaded rendering on by default (its hang and the m_gfxEopTick assert are
    # fixed, 18 Oct); configs saved before kept the old default, off.
    if config.get("version", 1) < 7:
        for entry in games.values():
            entry["multithreaded"] = True
    # Version 8 is upstream's (0.5.0: Drake's Deception's multithreading turned on once). Version
    # 7 above already turns it on in all three games, so 8 runs nothing here: a player's later
    # "off" stays off (21 Oct merge).
    for name in GAME_SETTING_DEFAULTS:
        config.pop(name, None)
    config["version"] = 8
    config["tab"] = section_name(config.get("tab"))  # the old tab names (24 Oct 2026 sidebar)
    # Under Wine the game folder is also kept as a Linux path: another prefix can show the same
    # folder under another drive letter (Launcher.game_dir).
    folder = config.get("game_dir")
    if WINE and folder and not config.get("game_dir_unix") and Path(folder).exists():
        config["game_dir_unix"] = unix_path(folder)
    return config


def write_text_safely(path, text):
    # Through a temporary file: a launcher and a game started from a shortcut may write at once, and
    # a write cut short must not leave half a file. (20 Oct 2026 audit: the probe-uN.ini writes
    # went in place and a kill mid-write reset display mode, FPS limit and upscaler.)
    temporary = path.with_name(f"{path.name}.{os.getpid()}.tmp")
    try:
        temporary.write_text(text, encoding="utf-8")
        os.replace(temporary, path)
    except OSError:
        # Windows refuses the swap while another process has the file open (a virus scanner, a
        # second launcher): written in place, as before.
        try:
            temporary.unlink(missing_ok=True)
        except OSError:
            pass
        path.write_text(text, encoding="utf-8")


def write_config(config):
    CONFIG_FILE.parent.mkdir(parents=True, exist_ok=True)
    write_text_safely(CONFIG_FILE, json.dumps(config, indent=2))


def readiness(game_ok, game_version, game_dir, update_found, update_version, update_problems, bridge_present,
              dlss_present, fg_present, slow_drive, release_version):
    """Home's "Ready to play" checklist (24 Oct 2026) from plain facts, so it is testable without Tk:
    a list of dicts (key, state ok|warn|missing|optional, title, detail, actions [(label, action key)]).
    Without a valid game folder the first-run steps (game, update, optional DLLs) are numbered."""
    items = []

    def add(key, state, title, detail, actions=()):
        items.append({"key": key, "state": state, "title": title, "detail": detail, "actions": list(actions)})
    if game_ok:
        add("game", "ok", "Game files", f"Version {game_version or '?'}  -  {game_dir}")
    else:
        add("game", "missing", "Choose your game folder",
            "The folder of your own copy of the Collection (CUSA02320 or CUSA02344), or install it from its .pkg.",
            [("Choose folder", "choose_game_dir"), ("Install from .pkg", "install_game_pkg")])
    fix_update = [("Choose update folder", "choose_update_folder"), ("Install from .pkg", "install_game_update")]
    if update_problems:
        more = " ..." if len(update_problems) > 3 else ""
        add("update", "warn", "Game update 1.02", "Missing or damaged files: " + "; ".join(update_problems[:3]) + more
            + ". Extract the update again, or install it from its .pkg.", fix_update)
    elif update_found:
        add("update", "ok", "Game update 1.02", f"Version {update_version or '?'} is used")
    elif game_version and game_version >= "01.02":
        add("update", "ok", "Game update 1.02", f"Version {game_version} (the update is in the game folder)")
    else:
        add("update", "warn" if game_ok else "missing", "Game update 1.02",
            "Most of the port's fixes, crash fixes among them, are written for update 1.02. Choose your extracted "
            "update folder, or install it from its .pkg.", fix_update)
    get = ("Get from NVIDIA", "get_nvidia")
    if bridge_present and not dlss_present:
        add("dlss", "optional", "NVIDIA DLSS",
            f"Optional, for NVIDIA RTX cards: adds DLSS to Anti-aliasing. Needs NVIDIA's {NGX_DLL}, not included.",
            [get, (f"Add {NGX_DLL}...", "add_dlss")])
    elif bridge_present:
        add("dlss", "ok", "DLSS ready", f"{NGX_DLL} is installed: choose DLSS under Anti-aliasing (Display).")
        if not fg_present:
            add("framegen", "optional", "NVIDIA DLSS Frame Generation",
                f"Optional, for RTX 40 or newer. Needs NVIDIA's {NGX_FG_DLL}, not included.",
                [get, (f"Add {NGX_FG_DLL}...", "add_framegen")])
    if slow_drive:
        add("drive", "warn", "Slow drive", f"{slow_drive}. Copy the port and the game to an internal SSD: on this "
            "drive the games freeze again and again while the port writes its shader cache, logs and saves.")
    if release_version:
        add("launcher", "warn", f"Launcher update {release_version} is available",
            "Saves and settings are kept; the launcher restarts.", [("Install", "install_update")])
    if not game_ok:
        steps = [item for item in items if item["key"] in ("game", "update", "dlss", "framegen")]
        for number, item in enumerate(steps, 1):
            item["title"] = f"{number}. {item['title']}"
    return items


class Launcher:
    def __init__(self, root, direct=None):
        self.root = root
        # Direct start (--play u1|u2|u3, GitHub #25): the game starts at once with the saved
        # settings, without the launcher window, which closes with the game.
        self.direct = direct
        self.s = max(1.0, root.winfo_fpixels("1i") / 96.0)
        self.config = read_config()
        if self.config.get("game") not in GAME_KEYS:
            self.config["game"] = "u1"
        for key in GAME_KEYS:
            if not controls_from_ini(read_ini(key), self.config["games"][key]):
                write_ini(key, controls_to_ini(self.config["games"][key]))
        self.reload_ini()
        self.process = None
        self.next_start_noted = False  # the "applies at the next start" toast shows once per run
        self.output = queue.Queue()
        self.last_output = 0.0
        self.game_started = False
        self.widgets = {}
        # The graphics card (as last detected; detect_gpu checks again) for the Home preset advice.
        self.gpu_name = self.config.get("gpu")
        self.recommended = gpu_tier(self.gpu_name)
        self.glyph_photos = {}
        self.fonts()
        root.title(f"{APP_TITLE} - {EDITION}")
        root.configure(bg=BG)
        root.resizable(True, True)
        try:
            root.iconbitmap(default=str(ASSETS / "uncharted.ico"))
        except tk.TclError:
            pass
        self.build()
        root.update_idletasks()
        self.fit_to_screen()
        self.restore_window_position()
        write_config(self.config)  # keeps one-time updates (read_config) even without a change
        if not direct:
            self.focus_first()
        root.protocol("WM_DELETE_WINDOW", self.close)
        root.after(150, self.poll)
        # Updates (launcher/updater.py): what an installed one left behind goes; a newer release
        # shows the UPDATE button.
        self.update_release = None
        self.update_busy = False
        self.update_result = None
        root.after(3000, lambda: updater.cleanup(ROOT))
        if self.config.get("check_updates", True) and not direct:
            root.after(1500, self.check_updates)
        if not direct:
            self.detect_gpu()
            self.in_background(vulkan_gpus, self.add_gpu_row)
        if direct:
            self.select_game(direct, save=False)
            root.after(100, self.play_direct)
        else:
            self.pad_nav = PadNav(self)
            self.pad_nav.start()

    def restore_window_position(self):
        """The window opens where it was last closed, if that spot is still on a screen."""
        position = self.config.get("window_xy")
        if not (isinstance(position, list) and len(position) == 2):
            return
        x, y = int(position[0]), int(position[1])
        self.root.update_idletasks()
        screen_w, screen_h = self.root.winfo_vrootwidth(), self.root.winfo_vrootheight()
        if -50 <= x <= max(0, screen_w - 200) and 0 <= y <= max(0, screen_h - 200):
            # All of the window on the screen (the title bar and the taskbar included).
            y = min(y, max(0, self.root.winfo_screenheight() - self.window_size[1] - self.px(70)))
            self.root.geometry(f"+{x}+{y}")

    def fit_to_screen(self):
        """About 1240x760, never more than the screen's work area: the section body flexes and
        scrolls, so the footer with PLAY stays in view (1280x720 at 150%, the Steam Deck's 1280x800)."""
        x, y, area_w, area_h = work_area(self.root)
        title_bar = self.px(40)
        min_w = min(self.root.winfo_reqwidth(), area_w - self.px(60))
        min_h = min(self.root.winfo_reqheight(), area_h - self.px(60))
        self.root.minsize(min_w, min_h)
        width = min(max(self.px(1240), min_w), area_w)
        height = min(max(self.px(760), min_h), area_h - title_bar)
        self.window_size = (width, height)
        self.root.geometry(f"{width}x{height}+{x + max(0, (area_w - width) // 2)}"
                           f"+{y + max(0, (area_h - height - title_bar) // 2)}")

    def detect_gpu(self):
        """Finds the graphics card in the background (Windows' list takes a moment) and marks the
        Home preset that suits it."""
        found = []
        threading.Thread(target=lambda: found.append(gpu_names()), daemon=True).start()

        def done():
            if not found:
                self.root.after(200, done)
                return
            names = found[0]
            if not names:
                return
            known = [name for name in names if gpu_tier(name)]
            name = max(known, key=lambda n: PRESET_ORDER.index(gpu_tier(n))) if known else names[0]
            self.gpu_name, self.recommended = name, gpu_tier(name)
            if self.config.get("gpu") != name:
                self.config["gpu"] = name
                write_config(self.config)
            if hasattr(self, "preset_cards"):
                self.redraw_presets()
        self.root.after(200, done)

    def px(self, value):
        return int(value * self.s)

    def glyph(self, name, size, color):
        """A tinted icon of the sheet (`name` in GLYPHS) at `size` px, as a PhotoImage kept for the window's life."""
        key = (name, size, color)
        if key not in self.glyph_photos:
            sheet = Image.open(ASSETS / "icons.png")
            cell = sheet.width // len(GLYPHS)
            left = GLYPHS.index(name) * cell
            alpha = sheet.crop((left, 0, left + cell, sheet.height)).getchannel("A").resize((size, size), Image.LANCZOS)
            image = Image.new("RGBA", (size, size), color)
            image.putalpha(alpha)
            self.glyph_photos[key] = ImageTk.PhotoImage(image)
        return self.glyph_photos[key]

    def fonts(self):
        # Headings in Bahnschrift (Windows 10 and later: the condensed lettering of maps and road
        # signs); Segoe UI where it is missing (Wine/Proton without it).
        families = set(tkfont.families(self.root))
        head = "Bahnschrift SemiBold" if "Bahnschrift SemiBold" in families else "Segoe UI Semibold"
        self.f_wordmark = (head, 22)
        self.f_subtitle = ("Segoe UI Semibold", 7)
        self.f_edition = (head, 8)
        self.f_caption = ("Segoe UI Semibold", 8)
        self.f_banner = (head, 20)
        self.f_heading = (head, 15)
        self.f_card_number = (head, 20)
        self.f_card_title = (head, 14)
        self.f_entry_title = (head, 11)
        self.f_body = ("Segoe UI", 10)
        self.f_body_bold = ("Segoe UI Semibold", 10)
        self.f_small = ("Segoe UI", 9)
        self.f_tab = ("Segoe UI Semibold", 10)
        self.f_play = (head, 17)

    # -- layout -----------------------------------------------------------------------------

    def build(self):
        # 24 Oct 2026: a sidebar (games, sections) beside a column with the banner, the section and
        # the footer. The old stack (hero, game cards, tabs) left 330 px for the settings and did
        # not fit 1280x720 (1080p at 150%).
        self.build_sidebar()
        tk.Frame(self.root, bg=LINE, width=1).pack(side="left", fill="y")
        self.main = tk.Frame(self.root, bg=BG)
        self.main.pack(side="left", fill="both", expand=True)
        self.build_hero()
        self.build_footer()
        self.build_tabs()
        self.select_game(self.config.get("game", "u1"), save=False)
        self.show_tab(self.config.get("tab"), save=False)
        self.refresh_dependencies()
        self.footer.lift()  # Tab goes through the settings before PLAY (the footer is built before the page)
        self.bind_keyboard()

    def focus_first(self):
        """The keyboard starts on PLAY, or on the first fix on Home when the game is not set up."""
        target = self.play_button if is_game_dir(self.game_dir()) else self.first_focusable(self.home_list)
        try:
            if target is not None:
                target.focus_set()
        except tk.TclError:
            pass

    def first_focusable(self, widget):
        for child in widget.winfo_children():
            if str(child.cget("takefocus")) == "1":
                return child
            found = self.first_focusable(child)
            if found is not None:
                return found

    def bind_keyboard(self):
        """24 Oct 2026: window shortcuts (Ctrl+1/2/3 game, Ctrl+Tab / Ctrl+Shift+Tab section, Ctrl+Enter
        PLAY) on the toplevel, not bind_all. Tk's own Tab binding (tag "all") beats the launcher's
        <KeyPress> there, so a Controls slot waiting for a key would see Tab move the focus: the
        toplevel's Tab bindings (they run before "all") hand it to on_key and stop."""
        root = self.root
        for number, game in enumerate(GAMES, 1):
            root.bind(f"<Control-Key-{number}>", self.shortcut(lambda k=game["key"]: self.select_game(k)))
        root.bind("<Control-Tab>", self.shortcut(lambda: self.step_section(1), tab=True))
        for sequence in ("<Control-Shift-Tab>", "<Control-ISO_Left_Tab>"):
            root.bind(sequence, self.shortcut(lambda: self.step_section(-1), tab=True))
        root.bind("<Control-Return>", self.shortcut(self.play_or_stop))
        for sequence in ("<Tab>", "<ISO_Left_Tab>"):
            root.bind(sequence, self.on_key)
        root.bind("<FocusIn>", self.on_focus, add="+")

    def shortcut(self, action, tab=False):
        """A window shortcut's handler. While a Controls slot waits for a key it is not a shortcut: the
        key is the binding (`tab`: the ones Tk would otherwise take for itself, so they stop here)."""
        def run(event=None):
            if self.capture_active():
                return self.capture_key(event) if tab else None
            if self.capturing is None:
                action()
            return "break"
        return run

    def step_section(self, step):
        names = [name for name, _ in SECTIONS]
        name = names[(names.index(self.section) + step) % len(names)]
        self.show_tab(name)
        self.tab_buttons[name][0].focus_set()  # the page it replaces may hold the focus

    def on_focus(self, event):
        """Tab to a control inside a scrolling page scrolls the page until the control is in view."""
        canvas = event.widget
        while isinstance(canvas, tk.Misc) and str(canvas) not in self.scroll_canvases:
            canvas = canvas.master
        if not isinstance(canvas, tk.Misc) or canvas is event.widget:
            return
        region = canvas.tk.splitlist(canvas.cget("scrollregion"))
        view = canvas.winfo_height()
        if len(region) != 4 or view <= 1:
            return
        top = event.widget.winfo_rooty() - canvas.winfo_rooty()
        bottom = top + event.widget.winfo_height()
        move = top if top < 0 else max(0, bottom - view)
        total = float(region[3]) - float(region[1])
        if move and total > view:
            wanted = min(max(canvas.canvasy(0) + move, float(region[1])), float(region[1]) + total - view)
            canvas.yview_moveto((wanted - float(region[1])) / total)

    def build_sidebar(self):
        bar = tk.Frame(self.root, bg=PANEL)
        bar.pack(side="left", fill="y")
        w, pad = self.px(240), self.px(20)
        brand = tk.Frame(bar, bg=PANEL)
        brand.pack(side="top", fill="x", padx=pad, pady=(self.px(18), self.px(10)))
        # The compass emblem (drawn in VectorCraft) beside the wordmark.
        emblem_size = self.px(46)
        self.emblem_photo = ImageTk.PhotoImage(
            Image.open(ASSETS / "emblem.png").convert("RGBA").resize((emblem_size, emblem_size), Image.LANCZOS))
        tk.Label(brand, image=self.emblem_photo, bg=PANEL, bd=0).pack(side="left", anchor="n", padx=(0, self.px(10)))
        titles = tk.Frame(brand, bg=PANEL)
        titles.pack(side="left", anchor="n")
        tk.Label(titles, text="UNCHARTED", font=self.f_wordmark, bg=PANEL, fg=TEXT, anchor="w", bd=0).pack(
            side="top", anchor="w")
        tk.Label(titles, text="THE NATHAN DRAKE COLLECTION", font=self.f_subtitle, bg=PANEL, fg=MUTED,
                 anchor="w", bd=0).pack(side="top", anchor="w", padx=(self.px(2), 0))
        # The fork's edition as a flare-orange tag under the subtitle.
        tk.Label(titles, text=EDITION.upper(), font=self.f_edition, bg=ACCENT, fg=INK, padx=self.px(8),
                 pady=self.px(1)).pack(side="top", anchor="w", padx=(self.px(2), 0), pady=(self.px(8), 0))
        # Bottom first: when the window is short, the section list gives way, not these.
        version = updater.installed_version(ROOT)
        tk.Label(bar, text=f"Version {updater.version_text(version)}" if version else "Source tree (updates with git)",
                 font=self.f_small, bg=PANEL, fg=DIM, anchor="w").pack(side="bottom", fill="x", padx=pad,
                                                                       pady=(0, self.px(14)))
        links = tk.Canvas(bar, width=w, height=self.px(40), bg=PANEL, highlightthickness=2,
                          highlightbackground=PANEL, highlightcolor=ACCENT, takefocus=1)
        links.pack(side="bottom")
        self.draw_links(links, w)
        on_activate(links, lambda: webbrowser.open(LINKS[0][1]))  # the one link

        def caption(text):
            tk.Label(bar, text=text, font=self.f_caption, bg=PANEL, fg=DIM, anchor="w").pack(
                side="top", fill="x", padx=pad, pady=(self.px(10), self.px(4)))
        caption("GAMES  ·  DOUBLE-CLICK TO PLAY")
        self.cards = {}
        for game in GAMES:
            h = self.px(48)
            canvas = tk.Canvas(bar, width=w, height=h, bg=PANEL, highlightthickness=2, highlightbackground=PANEL,
                               highlightcolor=ACCENT, takefocus=1, cursor="hand2")
            canvas.pack(side="top")
            canvas.bind("<Button-1>", lambda e, k=game["key"]: self.select_game(k))
            on_activate(canvas, lambda k=game["key"]: self.select_game(k))
            canvas.bind("<Double-Button-1>", lambda e, k=game["key"]: self.play_game(k))
            canvas.bind("<Enter>", lambda e, k=game["key"]: self.draw_card(k, hover=True))
            canvas.bind("<Leave>", lambda e, k=game["key"]: self.draw_card(k))
            self.cards[game["key"]] = (canvas, game, w, h)
        caption("SECTIONS")
        self.section = None
        self.tab_buttons = {}
        for name, label in SECTIONS:
            canvas = tk.Canvas(bar, width=w, height=self.px(34), bg=PANEL, highlightthickness=2,
                               highlightbackground=PANEL, highlightcolor=ACCENT, takefocus=1, cursor="hand2")
            canvas.pack(side="top")
            canvas.bind("<Button-1>", lambda e, n=name: self.show_tab(n))
            on_activate(canvas, lambda n=name: self.show_tab(n))
            canvas.bind("<Enter>", lambda e, n=name: self.draw_section(n, hover=True))
            canvas.bind("<Leave>", lambda e, n=name: self.draw_section(n))
            self.tab_buttons[name] = (canvas, label)
        tk.Misc.lift(links)  # last in the Tab order (it was built first to sit at the bottom); Canvas.lift is for items

    def draw_section(self, name, hover=False):
        canvas, label = self.tab_buttons[name]
        w, h = int(canvas["width"]), int(canvas["height"])
        selected = name == self.section
        canvas.delete("all")
        canvas.create_rectangle(0, 0, w, h, fill=PANEL_HI if selected or hover else PANEL, outline="")
        if selected:
            canvas.create_rectangle(0, 0, self.px(4), h, fill=ACCENT, outline="")
        canvas.create_image(self.px(22), h // 2, anchor="w",
                            image=self.glyph(name, self.px(18), ACCENT if selected else TEXT if hover else MUTED))
        canvas.create_text(self.px(52), h // 2, text=label, font=self.f_tab, anchor="w",
                           fill=TEXT if selected or hover else MUTED)

    def build_hero(self):
        """The banner over the section: the Collection's art (as the old hero drew it) with the
        selected game's name. Drawn again when the column gets wider or narrower."""
        self.banner_art = None
        for name in ("pic1.png", "pic0.png"):
            if (self.game_dir() / "sce_sys" / name).exists():
                self.banner_art = Image.open(self.game_dir() / "sce_sys" / name).convert("RGB")
                break
        self.banner = tk.Canvas(self.main, width=self.px(400), height=self.px(110), bg=BG, highlightthickness=0)
        self.banner.pack(side="top", fill="x")
        x = self.px(28)
        self.banner.create_text(x, self.px(44), text="", font=self.f_banner, fill=TEXT, anchor="w", tags="game")
        self.banner.create_text(x + self.px(1), self.px(74), text="Settings for this game are kept separately",
                                font=self.f_body, fill=MUTED, anchor="w")
        self.banner_job = None
        self.draw_banner(self.px(1000))  # direct mode never maps the window

        def resized(event):
            if abs(event.width - self.banner_width) >= self.px(40):
                if self.banner_job:
                    self.root.after_cancel(self.banner_job)
                self.banner_job = self.root.after(120, lambda: self.draw_banner(event.width))
        self.banner.bind("<Configure>", resized)

    def draw_banner(self, width):
        self.banner_job = None
        self.banner_width = width
        # A little wider than the column: a window widened by less than 40 px shows no gap.
        w, h = width + self.px(40), self.px(110)
        art = self.banner_art or Image.new("RGB", (1920, 1080), HERO_LIGHT)
        # Fill the width; keep Drake (right) in view.
        scale = w / art.width
        art = art.resize((w, max(h, int(art.height * scale))), Image.LANCZOS)
        top = int(art.height * 0.02)
        art = art.crop((0, top, w, top + h))
        # The art in two tones, night blue to a faded teal: the game's picture as a chart at night.
        art = ImageOps.colorize(ImageOps.autocontrast(art.convert("L"), cutoff=1), HERO_DARK, HERO_LIGHT)
        # Darker on the left, where the title sits.
        shade = Image.linear_gradient("L").rotate(90).resize((w, h))
        art = Image.composite(art, Image.new("RGB", (w, h), HERO_DARK), shade.point(lambda v: 90 + v * 165 // 255))
        # A map grid over it, then the fade into the dark panel below.
        draw = ImageDraw.Draw(art)
        step = self.px(34)
        for gx in range(step // 2, w, step):
            draw.line([(gx, 0), (gx, h)], fill=HERO_GRID)
        for gy in range(step // 2, h, step):
            draw.line([(0, gy), (w, gy)], fill=HERO_GRID)
        fade = Image.new("L", (w, h), 0)
        fade_draw = ImageDraw.Draw(fade)
        start = int(h * 0.55)
        for y in range(start, h):
            fade_draw.line([(0, y), (w, y)], fill=int(255 * ((y - start) / (h - start)) ** 1.4))
        art = Image.composite(Image.new("RGB", (w, h), BG), art, fade)
        self.banner_photo = ImageTk.PhotoImage(art)  # the last one only: the old image goes
        self.banner.delete("art")
        self.banner.create_image(0, 0, image=self.banner_photo, anchor="nw", tags="art")
        self.banner.tag_lower("art")

    def draw_links(self, canvas, width):
        """The fork's GitHub as a badge at the bottom of the sidebar, from the left (`width` unused
        since the badges moved there from the hero's top right)."""
        left, top, height = self.px(20), self.px(5), self.px(30)
        for label, url, color, hover in LINKS:
            tag = f"link-{label}"
            text = canvas.create_text(0, 0, text=label, font=self.f_body_bold, fill=TEXT, anchor="w", tags=tag)
            x1, _, x2, _ = canvas.bbox(text)
            badge_w = (x2 - x1) + self.px(46)
            right = left + badge_w
            mid = top + height // 2
            canvas.coords(text, left + self.px(34), mid)
            badge = round_rect(canvas, left, top, right, top + height, height // 2, fill=color, outline=HOVER_LINE,
                               tags=tag)
            canvas.tag_lower(badge, text)
            # Icon: GitHub's code brackets in the flare orange (the sheet's link glyph).
            canvas.create_image(left + self.px(20), mid, image=self.glyph("link", self.px(18), ACCENT), tags=tag)
            canvas.tag_bind(tag, "<Button-1>", lambda e, u=url: webbrowser.open(u))
            canvas.tag_bind(tag, "<Enter>", lambda e, b=badge, c=hover: (canvas.itemconfig(b, fill=c, outline=ACCENT),
                                                                       canvas.config(cursor="hand2")))
            canvas.tag_bind(tag, "<Leave>", lambda e, b=badge, c=color: (canvas.itemconfig(b, fill=c, outline=HOVER_LINE),
                                                                       canvas.config(cursor="")))
            left = right + self.px(10)

    def draw_card(self, key, hover=False):
        """A game in the sidebar: selected = the orange bar and the lighter panel."""
        canvas, game, w, h = self.cards[key]
        selected = key == self.config.get("game")
        canvas.delete("all")
        canvas.create_rectangle(0, 0, w, h, fill=PANEL_HI if selected or hover else PANEL, outline="")
        if selected:
            canvas.create_rectangle(0, 0, self.px(4), h, fill=ACCENT, outline="")
        canvas.create_text(self.px(22), h // 2, text=game["number"], font=self.f_card_number,
                           fill=ACCENT if selected else DIM, anchor="w")
        canvas.create_text(self.px(50), h // 2 - self.px(8), text=game["title"], font=self.f_entry_title,
                           fill=TEXT if selected or hover else MUTED, anchor="w")
        canvas.create_text(self.px(51), h // 2 + self.px(10), text=game["subtitle"], font=self.f_small,
                           fill=MUTED if selected else DIM, anchor="w")

    def select_game(self, key, save=True):
        if key not in self.cards:
            key = "u1"
        changed = key != self.config.get("game")
        if changed:  # a pending key or pad capture belongs to the game it started in
            self.stop_capture()
            if getattr(self, "pad_capture", None) is not None:
                self.stop_pad_capture()
        self.config["game"] = key
        for k in self.cards:
            self.draw_card(k)
        self.banner.itemconfig("game", text=self.current_game()["name"])
        if save:
            write_config(self.config)
        if changed:
            # Each game keeps its own settings: show the selected game's.
            self.reload_ini()
            self.refresh_controls()
        self.refresh_home()
        if hasattr(self, "status") and not self.process:
            self.set_status(f"Ready to play {self.current_game()['name']}.")

    def game_settings(self):
        """Preset, render resolution and multithreading of the selected game."""
        return self.config["games"][self.config["game"]]

    def save_ini(self, updates):
        write_ini(self.config["game"], updates)
        self.ini_seen = ini_mtime(self.config["game"])

    def reload_ini(self):
        """The selected game's settings from its file (the in-game menu writes it too)."""
        self.ini = read_ini(self.config["game"])
        self.ini_seen = ini_mtime(self.config["game"])
        if controls_from_ini(self.ini, self.game_settings()):
            write_config(self.config)

    def save_controls(self):
        """The selected game's controls, also into its settings file (the game reads them there)."""
        write_config(self.config)
        self.save_ini(controls_to_ini(self.game_settings()))

    def follow_ini(self):
        """Shows changes the in-game menu saved (Insert, then closing the menu)."""
        if ini_mtime(self.config["game"]) != getattr(self, "ini_seen", None):
            self.reload_ini()
            self.refresh_controls()

    def refresh_controls(self):
        """Every control shows the selected game's settings."""
        for key in INI_DEFAULTS:
            widget = self.widgets.get(key)
            if isinstance(widget, Toggle):
                widget.set(self.ini.get(key, INI_DEFAULTS[key]) == "1")
            elif isinstance(widget, (Segmented, Dropdown)):
                widget.set(self.ini.get(key, INI_DEFAULTS[key]))
        # The performance counter's items are Chip pills (overlay_panel): the loop above skips them.
        for key, widget in getattr(self, "overlay_chips", {}).items():
            if isinstance(widget, Chip):
                widget.set(self.ini.get(key, INI_DEFAULTS[key]) == "1")
        settings = self.game_settings()
        if isinstance(self.widgets.get("resolution"), Segmented):
            self.widgets["resolution"].set(settings["resolution"])
        self.refresh_render_size()
        if isinstance(self.widgets.get("aspect"), Segmented):
            self.widgets["aspect"].set(settings.get("aspect", "16:9"))
        if isinstance(self.widgets.get("multithreaded"), Toggle):
            self.widgets["multithreaded"].set(settings["multithreaded"])
        if isinstance(self.widgets.get("save_profile"), Segmented):
            self.widgets["save_profile"].set(str(settings.get("save_profile", "1")))
        if isinstance(self.widgets.get("mouse"), Toggle):
            self.widgets["mouse"].set(settings.get("mouse", True))
            self.widgets["mouse_sens"].set(settings.get("mouse_sens", "1.0"))
            self.widgets["mouse_invert"].set(settings.get("mouse_invert", False))
            self.widgets["mouse_aim_sens"].set(settings.get("mouse_aim_sens", "1.0"))
            self.widgets["mouse_vertical"].set(settings.get("mouse_vertical", "1.0"))
            self.widgets["mouse_invert_x"].set(settings.get("mouse_invert_x", False))
            self.widgets["vibration"].set(settings.get("vibration", True))
            self.widgets["prompts"].set(settings.get("prompts", "auto"))
            self.refresh_pad_slots()
        if hasattr(self, "binding_slots"):
            self.refresh_bindings()
        self.sync_sharpening()
        if hasattr(self, "preset_cards"):
            self.redraw_presets()
        self.update_fps_note()
        if hasattr(self, "ini_label"):
            self.ini_label.config(text=f"Settings: {ini_file(self.config['game'])}")
        self.refresh_dependencies()

    def refresh_dependencies(self):
        """Greys out the options that do nothing with the anti-aliasing now chosen and says what they
        need. Display only: the ini and the game's settings are not touched, so the values come back
        when the upscaler changes (here, or in the in-game menu through follow_ini)."""
        for key, (enabled, reason) in option_availability(self.ini, frame_gen_available()).items():
            widget, note = self.widgets.get(key), getattr(self, "row_notes", {}).get(key)
            if widget is not None and widget.winfo_exists():
                widget.set_enabled(enabled)
            if note is not None and note.winfo_exists():
                if enabled:
                    note.pack_forget()
                else:
                    note.config(text=reason)
                    if not note.winfo_manager():
                        note.pack(side="top", fill="x")

    def game_dir(self):
        folder = self.config.get("game_dir") or ""
        if not folder:
            return DEFAULT_GAME_DIR
        if WINE and not Path(folder).exists():
            # Chosen in another prefix (another Steam shortcut): the same folder by its Linux path.
            here = windows_path(self.config.get("game_dir_unix"))
            if here and Path(here).exists():
                return Path(here)
        return Path(folder)

    def choose_game_dir(self):
        from tkinter import filedialog
        folder = filedialog.askdirectory(
            title="Select the Uncharted game folder (the one with eboot.bin and sce_sys)",
            initialdir=str(self.game_dir() if self.game_dir().exists() else ROOT))
        if not folder:
            return False
        if not is_game_dir(folder):
            self.error("That folder has no eboot.bin and sce_sys/param.sfo.\n\n"
                       "Choose the game's root folder (CUSA02320 or CUSA02344).")
            return False
        self.config["game_dir"] = folder
        if WINE:
            self.config["game_dir_unix"] = unix_path(folder)
        write_config(self.config)
        if hasattr(self, "game_dir_label"):
            self.game_dir_label.config(text=f"Game files: {folder}")
            self.update_label.config(text=self.update_text())
        self.refresh_home()
        return True

    def chosen_update(self):
        """The update folder chosen in System (Choose update folder), if it is still there."""
        folder = self.config.get("update_dir") or ""
        if folder:
            for candidate in (Path(folder), Path(folder) / "uroot"):
                if is_game_dir(candidate):
                    return candidate
        return None

    def update_dir(self):
        """The update the games use: the chosen folder, else one next to the game folder."""
        return self.chosen_update() or game_update_dir(self.game_dir())

    def update_text(self):
        update = self.update_dir()
        if update:
            return f"Game update {app_version(update) or '?'} is used: {update}"
        folder = self.game_dir()
        version = app_version(folder)
        if version and version >= "01.02":
            return f"Game version {version} (the update is in the game folder)."
        title = folder.parent if folder.name.lower() == "uroot" else folder
        return (f"Game version {version or '?'}, no update found. For the 1.02 fixes, choose your update 1.02 "
                f"folder (or its .pkg) under GAME UPDATE.")

    def current_game(self):
        return next(g for g in GAMES if g["key"] == self.config.get("game"))

    def build_tabs(self):
        """The section's title and its body (packed after the footer: a short window shrinks the
        body, which scrolls, never the footer)."""
        outer = tk.Frame(self.main, bg=BG)
        outer.pack(side="top", fill="both", expand=True, padx=self.px(28))
        header = tk.Frame(outer, bg=BG)
        header.pack(side="top", fill="x", pady=(0, self.px(6)))
        self.section_title = tk.Label(header, text="", font=self.f_heading, bg=BG, fg=TEXT, anchor="w")
        self.section_title.pack(side="left", fill="x", expand=True)
        # Shown on Display and Performance only (show_tab).
        self.apply_all_button = self.flat_button(header, "Apply to all games", self.apply_to_all_games)
        tk.Frame(outer, bg=LINE, height=1).pack(side="top", fill="x")
        # The smallest body the window keeps (minsize): wide enough for the Controls columns.
        self.body = tk.Frame(outer, bg=BG, width=self.px(940), height=self.px(220))
        self.body.pack(side="top", fill="both", expand=True, pady=(self.px(12), self.px(10)))
        self.body.pack_propagate(False)
        # Pages taller than the body scroll (Display, Performance, System, Controls).
        self.scroll_canvases = set()
        self.rows = []  # every settings row built (title, hint, short, badge), for the tests
        self.row_notes = {}  # key -> the hidden "Needs ..." line of a row (see refresh_dependencies)
        # 24 Oct 2026: bound once for the whole session. Capturing a key used to bind_all and then
        # unbind_all these sequences, which also removed the page scrolling until the launcher restarted.
        self.capturing, self.capture_armed, self.capture_back = None, False, None
        self.root.bind_all("<MouseWheel>", self.on_wheel)
        self.root.bind_all("<KeyPress>", self.on_key)
        self.root.bind_all("<ButtonPress>", self.on_button)
        self.pages = {
            "home": self.page_setup(),
            "display": self.page_display(),
            "performance": self.page_performance(),
            "controls": self.page_controls(),
            "gameplay": self.page_cheats(),
            "system": self.page_advanced(),
        }

    def show_tab(self, name, save=True):
        """Shows a section (old tab names map to theirs: section_name)."""
        name = section_name(name)
        InfoTip.dismiss()
        if save and self.config.get("tab") != name:
            self.config["tab"] = name
            write_config(self.config)
        self.section = name
        for tab in self.tab_buttons:
            self.draw_section(tab)
        self.section_title.config(text=dict(SECTIONS)[name])
        if name in ("display", "performance"):
            self.apply_all_button.pack(side="right")
        else:
            self.apply_all_button.pack_forget()
        for tab, page in self.pages.items():
            holder = getattr(page, "holder", page)
            if tab == name:
                holder.pack(fill="both", expand=True)
            else:
                holder.pack_forget()

    def capture_active(self):
        return self.capturing is not None and self.capture_armed

    def on_wheel(self, event):
        InfoTip.dismiss()  # the page scrolls under an open popover
        # A Controls slot waiting for an input takes the wheel step as its binding.
        return self.capture_wheel(event) if self.capture_active() else self.wheel(event)

    def on_key(self, event):
        return self.capture_key(event) if self.capture_active() else None

    def on_button(self, event):
        InfoTip.dismiss(event.widget)  # a click elsewhere closes an open "?" popover
        return self.capture_button(event) if self.capture_active() else None

    def wheel(self, event):
        """The mouse wheel scrolls the page under the pointer, when it is taller than the window."""
        if getattr(self, "capturing", None) is not None:  # a Controls slot is waiting for an input
            return
        try:
            widget = self.root.winfo_containing(event.x_root, event.y_root)
        except (KeyError, tk.TclError):  # over a menu
            return
        while widget is not None:
            if str(widget) in self.scroll_canvases:
                if tuple(widget.yview()) != (0.0, 1.0):
                    step = int(-event.delta / 120) or (-1 if event.delta > 0 else 1)
                    widget.yview_scroll(step, "units")
                return
            widget = widget.master

    # -- pages ------------------------------------------------------------------------------

    def page(self, scroll=True):
        """A section's page: the frame its rows go in. With `scroll`, it scrolls (with a scrollbar)
        when the rows are taller than the body; show_tab packs its holder."""
        if not scroll:
            return tk.Frame(self.body, bg=BG)
        holder = tk.Frame(self.body, bg=BG)
        canvas = tk.Canvas(holder, bg=BG, highlightthickness=0)
        bar = ScrollBar(holder, self.s, canvas)
        inner = tk.Frame(canvas, bg=BG)
        window = canvas.create_window((0, 0), window=inner, anchor="nw")
        canvas.configure(yscrollcommand=bar.set)
        canvas.pack(side="left", fill="both", expand=True)

        def layout(event=None):
            width, height, need = canvas.winfo_width(), canvas.winfo_height(), inner.winfo_reqheight()
            if height <= 1:  # not shown yet
                return
            canvas.itemconfigure(window, width=width)
            canvas.configure(scrollregion=(0, 0, width, max(need, height)))
            if need > height and not bar.winfo_manager():
                bar.pack(side="right", fill="y", padx=(self.px(8), 0), before=canvas)
            elif need <= height and bar.winfo_manager():
                bar.pack_forget()
                canvas.yview_moveto(0)
        inner.bind("<Configure>", layout)
        canvas.bind("<Configure>", layout)
        self.scroll_canvases.add(str(canvas))
        inner.holder = holder
        return inner

    def on_resize(self, widget, handler):
        """handler(width) when the shown widget gets another width. Not while hidden: a page not
        shown takes the width its rows ask for, so a handler that changes that would loop."""
        def run(event=None):
            if widget.winfo_viewable():  # winfo_ismapped is also true inside a page not shown
                handler(widget.winfo_width())
        widget.bind("<Configure>", run, add="+")
        widget.bind("<Map>", run, add="+")

    def wrap(self, label):
        """The label's text wraps at the label's own width, which follows the window's."""
        self.on_resize(label, lambda width: label.config(wraplength=max(1, width - self.px(4))))
        return label

    def row(self, page, title, hint, control_factory, short=None, badge=None, note_key=None):
        """A setting: title and hint, the control right of them, or under them when it would take
        more than 45% of the row (long choice lists, graphics card names); decided again whenever
        the row's width changes. 24 Oct 2026: with `short`, the row shows that one line and a ?
        (InfoTip) after the title opens the full `hint`; a hint over HINT_MAX without a `short` is
        a bug (test_every_long_hint_has_a_short_line). `badge` ("ALL GAMES") sits after the title.
        With `note_key`, the row gets a hidden note line (self.row_notes[note_key]) that
        refresh_dependencies shows, under the hint, while the setting does nothing."""
        self.rows.append({"title": title, "hint": hint, "short": short, "badge": badge})
        row = tk.Frame(page, bg=BG)
        row.pack(side="top", fill="x", pady=(0, self.px(10)))
        text = tk.Frame(row, bg=BG)
        head = tk.Frame(text, bg=BG)
        head.pack(side="top", fill="x")
        tk.Label(head, text=title, font=self.f_body_bold, bg=BG, fg=TEXT, anchor="w").pack(side="left")
        if badge:
            Badge(head, badge, self.s, self.f_caption).pack(side="left", padx=(self.px(8), 0))
        if short:
            InfoTip(head, hint, self.s, self.f_small).pack(side="left", padx=(self.px(8), 0))
        if short or hint:
            self.wrap(tk.Label(text, text=short or hint, font=self.f_small, bg=BG, fg=MUTED, anchor="w",
                               justify="left", wraplength=self.px(560))).pack(side="top", fill="x")
        if note_key:
            self.row_notes[note_key] = self.wrap(tk.Label(text, text="", font=self.f_small, bg=BG, fg=ACCENT,
                                                          anchor="w", justify="left", wraplength=self.px(560)))
        control = control_factory(row)
        state = {"below": None}

        def place(below):
            state["below"] = below
            text.pack_forget()
            control.pack_forget()
            if below:
                text.pack(side="top", fill="x")
                control.pack(side="top", anchor="w", pady=(self.px(6), 0))
            else:  # the control first: the text takes what is left and wraps in it
                control.pack(side="right", padx=(self.px(20), 0))
                text.pack(side="left", fill="x", expand=True)

        def resized(width):
            below = control.winfo_reqwidth() > 0.45 * width
            if below != state["below"]:
                place(below)
        place(False)
        self.on_resize(row, resized)
        return control

    def ini_toggle(self, page, key, title, hint, **row_args):
        def factory(parent):
            return Toggle(parent, self.s, self.ini.get(key, "0") == "1",
                          lambda v, k=key: self.change(k, "1" if v else "0"))
        self.widgets[key] = self.row(page, title, hint, factory, **row_args)

    def ini_choice(self, page, key, title, hint, options, recommended=None, **row_args):
        def factory(parent):
            value, command = self.ini.get(key, INI_DEFAULTS.get(key)), lambda v, k=key: self.change(k, v)
            if len(options) > 9:  # a menu for long lists (frame rate limits: 10, with Frame Generation 16)
                return Dropdown(parent, [(o, f"{label}  ★" if o == recommended else label) for o, label in options],
                                value, command, self.f_small)
            return Segmented(parent, options, value, command, self.f_small, recommended)
        self.widgets[key] = self.row(page, title, hint, factory, note_key=key if key in DEPENDENT_OPTIONS else None,
                                     **row_args)

    def page_setup(self):
        page = self.page()
        # 24 Oct 2026: the readiness checklist (what the game needs, with the buttons that fix it)
        # above the presets; refresh_home fills it.
        self.home_checks, self.home_busy, self.home_again, self.home_shown = None, False, False, None
        panel = tk.Frame(page, bg=BG, highlightthickness=1, highlightbackground=LINE)
        panel.pack(side="top", fill="x", pady=(0, self.px(14)))
        head = tk.Frame(panel, bg=BG)
        head.pack(side="top", fill="x", padx=self.px(14), pady=(self.px(10), self.px(6)))
        tk.Label(head, text="READY TO PLAY", font=self.f_caption, bg=BG, fg=MUTED).pack(side="left")
        self.home_status = tk.Label(head, text="", font=self.f_body_bold, bg=BG, fg=GOOD)
        self.home_status.pack(side="left", padx=(self.px(12), 0))
        self.home_list = tk.Frame(panel, bg=BG)
        self.home_list.pack(side="top", fill="x", padx=self.px(14), pady=(0, self.px(4)))
        self.setup_intro = self.wrap(tk.Label(page, text="", font=self.f_body, bg=BG, fg=MUTED, anchor="w",
                                              justify="left"))
        self.setup_intro.pack(side="top", fill="x", pady=(0, self.px(12)))
        grid = tk.Frame(page, bg=BG)
        grid.pack(side="top", fill="x")
        self.preset_cards = {}
        card_h = self.px(150)

        def resized(name, width):
            canvas, w, h = self.preset_cards[name]
            if width != w:
                self.preset_cards[name] = (canvas, width, h)
                self.draw_preset(name)
        # Four cards that share the column's width (spacer columns between them).
        for i, name in enumerate(PRESET_ORDER):
            canvas = tk.Canvas(grid, width=self.px(150), height=card_h, bg=BG, highlightthickness=2,
                               highlightbackground=BG, highlightcolor=ACCENT, takefocus=1, cursor="hand2")
            canvas.grid(row=0, column=2 * i, sticky="ew")
            on_activate(canvas, lambda n=name: self.apply_preset(n))
            grid.columnconfigure(2 * i, weight=1, uniform="preset")
            if i:
                grid.columnconfigure(2 * i - 1, minsize=self.px(16))
            canvas.bind("<Button-1>", lambda e, n=name: self.apply_preset(n))
            canvas.bind("<Enter>", lambda e, n=name: self.draw_preset(n, hover=True))
            canvas.bind("<Leave>", lambda e, n=name: self.draw_preset(n))
            canvas.bind("<Configure>", lambda e, n=name: resized(n, e.width - 4))  # less the focus ring's border
            self.preset_cards[name] = (canvas, self.px(150), card_h)
        self.custom_note = tk.Label(page, text="", font=self.f_small, bg=BG, fg=MUTED, anchor="w")
        self.custom_note.pack(side="top", fill="x", pady=(self.px(10), 0))
        self.root.after_idle(self.redraw_presets)
        return page

    def draw_preset(self, name, hover=False):
        canvas, w, h = self.preset_cards[name]
        preset = PRESETS[name]
        selected = self.game_settings().get("preset") == name
        canvas.delete("all")
        round_rect(canvas, 2, 2, w - 2, h - 2, self.px(10), fill=PANEL_HI if selected or hover else PANEL,
                   outline=ACCENT if selected else (HOVER_LINE if hover else LINE), width=2 if selected else 1)
        canvas.create_text(self.px(20), self.px(26), text=preset["title"], font=self.f_card_title,
                           fill=ACCENT if selected else TEXT, anchor="w")
        canvas.create_text(self.px(20), self.px(50), text=preset["summary"], font=self.f_body_bold,
                           fill=TEXT, anchor="w")
        canvas.create_text(self.px(20), self.px(70), text=preset["detail"], font=self.f_small, fill=MUTED,
                           anchor="nw", width=w - self.px(40))
        if name == self.recommended:
            badge = canvas.create_text(w - self.px(20), self.px(26), text="RECOMMENDED", font=("Segoe UI Semibold", 7),
                                       fill=INK, anchor="e")
            x1, y1, x2, y2 = canvas.bbox(badge)
            pill = round_rect(canvas, x1 - self.px(8), y1 - self.px(3), x2 + self.px(8), y2 + self.px(3), self.px(8),
                              fill=ACCENT, outline=ACCENT)
            canvas.tag_lower(pill, badge)

    def redraw_presets(self):
        for name in self.preset_cards:
            self.draw_preset(name)
        if self.recommended:
            self.setup_intro.config(text=f"Recommended for your graphics card ({self.gpu_name}): "
                                         f"{PRESETS[self.recommended]['title'].title()}. Every setting can still be "
                                         "changed in Display and Performance.")
        else:
            self.setup_intro.config(text="Pick a quality level. Every setting can still be changed in Display "
                                         "and Performance.")
        custom = self.game_settings().get("preset") not in PRESETS
        name = self.current_game()["name"]
        self.custom_note.config(text=f"{name}: custom settings (changed by hand)." if custom
                                else f"Settings are kept per game: these are {name}'s.")

    def home_items(self):
        """The checklist from the state now. The checks that read archives or ask the drive's bus
        (update files, slow drive) come from the last background run (refresh_home), shown only
        while they are for the same folders."""
        game = self.game_dir()
        game_ok = is_game_dir(game)
        update = self.update_dir()
        key = (str(update), str(game))
        checks = self.home_checks if self.home_checks and self.home_checks[0] == key else (key, [], None)
        release = getattr(self, "update_release", None)
        return readiness(game_ok, app_version(game) if game_ok else None, str(game), bool(update),
                         app_version(update) if update else None, checks[1], dlss_bridge_present(),
                         dlss_available(), frame_gen_available(), checks[2],
                         updater.version_text(release["version"]) if release else None)

    def refresh_home(self):
        """Shows the checklist at once and runs the slow checks in the background (one at a time;
        asked again if state changed meanwhile). Call it after anything that changes the game folder,
        the update, an NVIDIA DLL or the launcher version."""
        if self.direct:
            return
        self.render_home()
        if self.home_busy:
            self.home_again = True
            return
        self.home_busy = True
        game, update = self.game_dir(), self.update_dir()

        def work():
            problems = []
            if update:
                try:
                    problems = self.update_installer().check_update(update)
                except Exception:  # the check must never break the launcher
                    problems = []
            places = []
            for label, folder in (("This port", ROOT), ("The game", game)):
                try:
                    reason = slow_drive_reason(folder) if folder.exists() else None
                except (OSError, ValueError, ctypes.ArgumentError):
                    reason = None
                if reason:
                    places.append(f"{label}: {reason}")
            return (str(update), str(game)), problems, "; ".join(places) or None

        def done(result):
            self.home_checks, self.home_busy = result, False
            self.render_home()
            if self.home_again:
                self.home_again = False
                self.refresh_home()
        self.in_background(work, done)

    def render_home(self):
        items = self.home_items()
        if items == self.home_shown:
            return
        self.home_shown = items
        for child in self.home_list.winfo_children():
            child.destroy()
        attention = sum(item["state"] in ("missing", "warn") for item in items)
        self.home_status.config(text=f"{attention} to fix" if attention else "✓  Ready to play",
                                fg=ACCENT if attention else GOOD)
        colors = {"ok": GOOD, "warn": ACCENT, "missing": STOP_RED, "optional": MUTED}
        actions = {"choose_game_dir": self.choose_game_dir, "install_game_pkg": self.install_game_pkg,
                   "choose_update_folder": self.choose_update_folder, "install_game_update": self.install_game_update,
                   "get_nvidia": lambda: webbrowser.open(NGX_DOWNLOAD_URL), "install_update": self.install_update,
                   "add_dlss": lambda: self.add_ngx(NGX_DLL), "add_framegen": lambda: self.add_ngx(NGX_FG_DLL)}
        for item in items:
            row = tk.Frame(self.home_list, bg=BG)
            row.pack(side="top", fill="x", pady=(0, self.px(8)))
            tk.Label(row, text="●", font=self.f_small, bg=BG, fg=colors[item["state"]]).pack(
                side="left", anchor="n", padx=(0, self.px(10)), pady=(self.px(2), 0))
            if item["actions"]:
                buttons = tk.Frame(row, bg=BG)
                buttons.pack(side="right", anchor="n", padx=(self.px(12), 0))
                for label, key in item["actions"]:
                    self.flat_button(buttons, label, actions[key]).pack(side="left", padx=(self.px(8), 0))
            text = tk.Frame(row, bg=BG)
            text.pack(side="left", fill="x", expand=True)
            tk.Label(text, text=item["title"], font=self.f_body_bold, bg=BG, fg=TEXT, anchor="w").pack(
                side="top", fill="x")
            self.wrap(tk.Label(text, text=item["detail"], font=self.f_small, bg=BG, fg=MUTED, anchor="w",
                               justify="left", wraplength=self.px(420))).pack(side="top", fill="x")

    def apply_preset(self, name):
        settings = self.game_settings()
        settings["resolution"] = PRESETS[name]["resolution"]
        if "resolution" in self.widgets:
            self.widgets["resolution"].set(settings["resolution"])
        self.refresh_render_size()
        for key, value in preset_values(name).items():
            self.ini[key] = value
            widget = self.widgets.get(key)
            if isinstance(widget, Toggle):
                widget.set(value == "1")
            elif isinstance(widget, (Segmented, Dropdown)):
                widget.set(value)
        self.sync_sharpening()
        self.refresh_dependencies()  # a preset can change the upscaler
        settings["preset"] = name
        write_config(self.config)
        self.save_ini({k: self.ini[k] for k in INI_DEFAULTS})
        self.redraw_presets()
        self.update_fps_note()

    def page_display(self):
        page = self.page()

        def resolution(parent):
            return Segmented(parent, resolution_options(self.game_settings().get("aspect", "16:9")),
                             self.game_settings()["resolution"], self.change_resolution, self.f_small)
        self.widgets["resolution"] = self.row(
            page, "Render resolution",
            "How sharp the picture is; higher needs a faster graphics card (the PS4 drew 1080p). If a "
            "game crashes above 1080p, go back to 1080p. Drake's Fortune draws at most 1620p and "
            "scales up.", resolution,
            short="How sharp the picture is; higher needs a faster graphics card.")

        def aspect(parent):
            return Segmented(parent, ASPECTS, self.game_settings().get("aspect", "16:9"),
                             self.change_aspect, self.f_small)
        self.widgets["aspect"] = self.row(
            page, "Aspect ratio",
            "For ultrawide monitors (21:9, 32:9), with Borderless or Fullscreen: the games show more at "
            "the sides and the same view top to bottom as on a 16:9 screen. 16:10 (Steam Deck: 800p is "
            "1280x800) fills the screen and shows more at the top and bottom.", aspect,
            short="Match your monitor's shape: ultrawide shows more at the sides, 16:10 more top and bottom.")
        self.render_size_note = tk.Label(page, text="", font=self.f_small, bg=BG, fg=ACCENT, anchor="w")
        self.render_size_note.pack(side="top", fill="x", pady=(0, self.px(10)))
        self.refresh_render_size()
        self.ini_choice(page, "upscaler", "Anti-aliasing",
                        "Smooths jagged, shimmering edges. TAA is the lightest; FSR and DLSS (NVIDIA RTX "
                        "cards) keep the most detail. Also in the in-game menu, where it changes at once.",
                        anti_aliasing_options(),
                        short="Smooths jagged, shimmering edges. TAA is the lightest; FSR and DLSS keep the most detail.")
        self.ini_choice(page, "preset", "Upscaling mode",
                        "FSR and DLSS only. Native / DLAA smooths edges at the render resolution; the other "
                        "modes draw the scene smaller and rebuild it at the render resolution (set it to your "
                        "screen's): more FPS, a little softer. Ultra Quality draws 71% of the width and height "
                        "(DLSS model M at 4K: about 24% more FPS than DLAA), Quality 67% (about 31%).",
                        UPSCALE_MODES,
                        short="FSR and DLSS only: drawing the scene smaller gives more FPS and a slightly softer picture.")
        self.ini_choice(page, "dlss_model", "DLSS model",
                        "NVIDIA's transformer models. K is the fastest and NVIDIA's pick for Native / DLAA, "
                        "Quality and Balanced; M is made for Performance mode. With DLAA at 4K, M costs about "
                        "5.7 ms more per frame than K (RTX 5070); in Performance mode about 0.9 ms.",
                        DLSS_MODELS, recommended="k",
                        short="NVIDIA's transformer models: K is the fastest, M is made for Performance mode.")
        self.ini_choice(page, "frame_gen", "DLSS Frame Generation",
                        "Adds one AI-generated frame between every two: about twice the frames shown, with a "
                        "little more input delay. Needs an RTX 40 or newer, an anti-aliasing mode (TAA, FSR or "
                        "DLSS) and NVIDIA's nvngx_dlssg.dll (add it under Ready to play on Home). The frame rate limit counts the "
                        "frames shown. Frames are only added while the game window has the focus, and it "
                        "turns Reflex on. Also in the in-game menu.",
                        [("0", "Off"), ("1", "On")], recommended="0",
                        short="Adds one AI-generated frame between every two: about twice the frames shown.")
        self.ini_choice(page, "display_mode", "Display mode",
                        "A normal window, a borderless window (quick Alt+Tab) or fullscreen.",
                        [("windowed", "Windowed"), ("borderless", "Borderless"), ("fullscreen", "Fullscreen")])
        self.ini_toggle(page, "vsync", "VSync", "Stops tearing (a line across the picture when the camera turns). "
                                               "Recommended on.")
        self.ini_toggle(page, "fsr1", "Upscale with FSR 1",
                        "When the game draws below your screen's resolution, makes the scaled-up picture sharper.")

        def sharpening(parent):
            value = self.ini.get("rcas_strength", "75") if self.ini.get("rcas", "1") == "1" else "off"
            return Segmented(parent, [("off", "Off"), ("25", "25%"), ("50", "50%"), ("75", "75%"), ("100", "100%")],
                             value, self.change_sharpening, self.f_small)
        self.widgets["sharpening"] = self.row(page, "Sharpening", "Makes the final picture crisper (AMD FidelityFX RCAS).",
                                              sharpening)
        self.ini_choice(page, "anisotropy", "Texture filtering",
                        "Sharper textures on floors and walls seen at an angle (anisotropic filtering). Game's is "
                        "how the PS4 drew them.",
                        [("0", "Game's"), ("4", "4x"), ("8", "8x"), ("16", "16x")], recommended="16",
                        short="Sharper textures on floors and walls seen at an angle. Game's is the PS4 look.")
        self.ini_toggle(page, "show_fps", "Show performance counter",
                        "FPS, temperatures and memory in a corner of the screen. Insert, then the Overlay "
                        "tab, changes it in the game too.",
                        short="FPS, temperatures and memory in a corner of the screen.")
        self.overlay_panel(page)
        self.display_page = page  # the Graphics card row joins it when the Vulkan list arrives
        return page

    def add_gpu_row(self, gpus):
        # 24 Oct 2026: the Vulkan device list (a child process, up to 10 s) is read in the background
        # at start; with more than one card the row is added at the end of the Display page.
        if len(gpus) > 1:
            self.ini_choice(self.display_page, "gpu_id", "Graphics card",
                            "Which GPU runs the game. Automatic picks the fastest one. Takes effect the next "
                            "time the game starts.",
                            [("-1", "Automatic")] + list(gpus), recommended="-1")

    def add_ngx(self, dll=NGX_DLL):
        from tkinter import filedialog
        source = filedialog.askopenfilename(
            title=f"Select {dll} downloaded from NVIDIA",
            filetypes=[(dll, dll), ("DLL files", "*.dll")],
            initialdir=str(Path.home() / "Downloads"))
        if not source:
            return
        source = Path(source)
        if source.name.lower() != dll:
            self.error(f"Choose the file named {dll}.")
            return
        # 24 Oct 2026: the PowerShell signature check (up to 60 s) runs off the Tk thread.
        self.set_status("Checking NVIDIA's signature...", "")

        def done(signed):
            if not self.process:
                self.set_status(f"Ready to play {self.current_game()['name']}.")
            if not signed:
                self.error(f"{source.name} has no valid NVIDIA signature. Download it again "
                           "from NVIDIA's DLSS page.")
                return
            try:
                shutil.copy2(source, ROOT / "gow3_pc" / "out" / dll)
            except OSError as error:
                self.error(f"Could not copy {dll}: {error}")
                return
            # 24 Oct 2026: the lists that gain entries (DLSS, the 144-240 FPS limits) are built again
            # in place; before, the launcher had to be reopened.
            self.rebuild_sections()
            self.refresh_home()
            if dll == NGX_FG_DLL:
                self.toast("NVIDIA DLSS Frame Generation is ready. Switch it on under Display.", "ok")
                return
            self.toast("NVIDIA DLSS is ready. DLSS is now listed under Anti-aliasing (Display).", "ok")
        self.in_background(lambda: ngx_signature_ok(source), done)

    def rebuild_sections(self):
        """Display and Performance built again from the current state, keeping the section shown
        (their option lists depend on the DLLs beside the runtime)."""
        for name, build in (("display", self.page_display), ("performance", self.page_performance)):
            holder = self.pages[name].holder
            self.scroll_canvases -= {str(w) for w in holder.winfo_children() if isinstance(w, tk.Canvas)}
            holder.destroy()
            self.widgets = {k: w for k, w in self.widgets.items() if w.winfo_exists()}
            self.pages[name] = build()  # page_display also points self.display_page at the new page
        self.add_gpu_row(_VULKAN_GPUS or [])
        self.refresh_controls()
        self.show_tab(self.section, save=False)

    # The performance counter's items: the ini key and the label on its pill.
    OVERLAY_ITEMS = (("overlay_fps", "FPS"), ("overlay_frametime", "Frame time"), ("overlay_gpu", "GPU"),
                     ("overlay_cpu", "CPU"), ("overlay_ram", "RAM"), ("overlay_vram", "VRAM"),
                     ("overlay_upscaler", "Upscaler"))

    def overlay_panel(self, page):
        """What the performance counter shows, where and how big (Display section; probe-u*.ini's
        show_fps and overlay_* keys, the ones the in-game menu's Overlay tab writes). It is built
        on its own: an error here leaves the rest of the page and the launcher working."""
        try:
            self._overlay_panel(page)
        except Exception:
            log_launcher_error(*sys.exc_info())
            self.overlay_chips = {}

    def _overlay_panel(self, page):
        def items(parent):
            box = tk.Frame(parent, bg=BG)
            self.overlay_chips = {}
            for key, label in self.OVERLAY_ITEMS:
                chip = Chip(box, self.s, self.f_small, label,
                            self.ini.get(key, INI_DEFAULTS[key]) == "1",
                            lambda v, k=key: self.change(k, "1" if v else "0"))
                chip.pack(side="left", padx=(self.px(6), 0))
                self.overlay_chips[key] = chip
            return box
        self.row(page, "What it shows",
                 "The frame rate, the frame time in milliseconds, the graphics card and the processor "
                 "with their temperatures, and memory. Pick any; with nothing picked the counter is "
                 "just its box.", items,
                 short="Frame rate, frame time, GPU and CPU with temperatures, memory: pick any.")
        self.ini_choice(page, "overlay_corner", "Counter corner", "Where the counter sits.",
                        [("0", "Top left"), ("1", "Top right"), ("2", "Bottom left"),
                         ("3", "Bottom right")])
        self.ini_choice(page, "overlay_layout", "Counter layout", "One line, or one item per line.",
                        [("0", "One line"), ("1", "Stacked")])
        self.ini_choice(page, "overlay_scale", "Counter size", "Text size of the counter.",
                        [("75", "75%"), ("100", "100%"), ("125", "125%"), ("150", "150%"),
                         ("200", "200%")])
        self.ini_choice(page, "overlay_opacity", "Counter background",
                        "How dark the counter's box is. None leaves the text on the picture.",
                        [("0", "None"), ("25", "25%"), ("50", "50%"), ("75", "75%"), ("100", "100%")])

    def change_sharpening(self, value):
        if value == "off":
            self.change("rcas", "0")
        else:
            self.ini["rcas"] = "1"
            self.change("rcas_strength", value)
            self.change("rcas", "1")

    def sync_sharpening(self):
        widget = self.widgets.get("sharpening")
        if widget:
            widget.set(self.ini.get("rcas_strength", "75") if self.ini.get("rcas", "1") == "1" else "off")

    def page_performance(self):
        page = self.page()
        tk.Label(page, text="★ marks the recommended choice.", font=self.f_small, bg=BG, fg=DIM,
                 anchor="w").pack(side="top", fill="x", pady=(0, self.px(6)))
        self.ini_choice(page, "fps_limit", "Frame rate limit",
                        "60 is how the games were made. Higher is smoother but experimental: if the game runs too "
                        "fast or Nate falls through the floor, go back to 60. With VSync, your monitor's refresh "
                        f"rate ({monitor_refresh_hz()} Hz) is the smoothest."
                        + (" 144 and above count the frames shown and need DLSS Frame Generation switched on "
                           "(Display); with it off the game stays at 120." if frame_gen_available() else ""),
                        [(str(v), label) for v, label in fps_limits()], recommended="60",
                        short="60 is how the games were made; higher is smoother but experimental.")
        self.ini_choice(page, "frames_queued", "Frames queued",
                        "Keep 1. 2 can add a few FPS in heavy scenes, but in Uncharted 2 Nate and other "
                        "characters then float a little above the floor.",
                        [("1", "1"), ("2", "2"), ("0", "Unlimited")], recommended="1",
                        short="Keep 1. 2 can add a few FPS, but in Uncharted 2 characters float above the floor.")
        self.ini_choice(page, "reflex", "NVIDIA Reflex",
                        "NVIDIA RTX cards. Lets the driver time the start of each frame. When the graphics card "
                        "limits the frame rate (Uncharted 3 at 4K) it cuts input delay by about 10 ms but costs "
                        "about 20% of the frame rate; when the processor limits it (1080p) it changes nothing. "
                        "Keep it off.",
                        [("0", "Off"), ("1", "On"), ("2", "On + Boost")], recommended="0",
                        short="NVIDIA RTX cards: less input delay, but it can cost frame rate. Keep it off.")
        self.ini_toggle(page, "async_shaders", "Compile shaders in the background",
                        "Fewer stutters when you reach new areas. Recommended on.")

        def multithreaded(parent):
            return Toggle(parent, self.s, self.game_settings()["multithreaded"],
                          lambda v: self.change_game_setting("multithreaded", v))
        self.widgets["multithreaded"] = self.row(
            page, "Multithreaded rendering (experimental)",
            "Prepares the picture on a second CPU core: more FPS on some PCs, mostly in Uncharted 2 and 3 "
            "(on by default; keep Frames queued at 1 with it). Turn it off if a game hangs while loading, or "
            "in Drake's Fortune if its guns flicker grey.", multithreaded,
            short="Prepares the picture on a second CPU core: more FPS on some PCs. Turn off if a game hangs.")

        def job_threads(parent):
            value = "off" if self.config.get("job_sleep", "auto") == "off" else "on"
            return Segmented(parent, (("on", "Sleep"), ("off", "Spin")), value,
                             lambda v: self.change_config("job_sleep", v), self.f_small, recommended="on")
        self.row(page, "Idle game threads",
                 "Sleep: the game's helper threads rest while they wait, which keeps 4-6 core CPUs fast "
                 "and saves power. Spin keeps their cores busy, as on the PS4.", job_threads,
                 short="Sleep lets the game's helper threads rest and saves power; Spin keeps them busy, as on the PS4.",
                 badge=ALL_GAMES)

        self.fps_note = tk.Label(page, text="", font=self.f_small, bg=BG, fg=ACCENT, anchor="w")
        self.fps_note.pack(side="top", fill="x")
        self.update_fps_note()
        return page

    def update_fps_note(self):
        if not hasattr(self, "fps_note"):
            return
        limit = self.ini.get("fps_limit", "60")
        hz = monitor_refresh_hz()
        if int(limit) > 60:
            self.fps_note.config(text=f"Frame rate unlocked up to {limit} FPS (your monitor: {hz} Hz). "
                                      "Applied when the game starts.")
        else:
            self.fps_note.config(text="")

    def page_controls(self):
        page = self.page(scroll=False)
        settings = self.game_settings()
        top = tk.Frame(page, bg=BG)
        top.pack(side="top", fill="x")
        tk.Label(top, text="Mouse", font=self.f_body_bold, bg=BG, fg=TEXT).pack(side="left")
        self.widgets["mouse"] = Toggle(top, self.s, settings.get("mouse", True),
                                       lambda v: self.change_game_setting("mouse", v))
        self.widgets["mouse"].pack(side="left", padx=(self.px(8), self.px(22)))
        tk.Label(top, text="Sensitivity", font=self.f_body_bold, bg=BG, fg=TEXT).pack(side="left")
        self.widgets["mouse_sens"] = Segmented(top, MOUSE_SENSITIVITIES, settings.get("mouse_sens", "1.0"),
                                               lambda v: self.change_game_setting("mouse_sens", v), self.f_small)
        self.widgets["mouse_sens"].pack(side="left", padx=(self.px(8), self.px(22)))
        tk.Label(top, text="Invert Y", font=self.f_body_bold, bg=BG, fg=TEXT).pack(side="left")
        self.widgets["mouse_invert"] = Toggle(top, self.s, settings.get("mouse_invert", False),
                                              lambda v: self.change_game_setting("mouse_invert", v))
        self.widgets["mouse_invert"].pack(side="left", padx=(self.px(8), 0))
        self.flat_button(top, "Reset controls", self.reset_controls).pack(side="right")
        aim = tk.Frame(page, bg=BG)
        aim.pack(side="top", fill="x", pady=(self.px(10), 0))
        tk.Label(aim, text="Aiming", font=self.f_body_bold, bg=BG, fg=TEXT).pack(side="left")
        self.widgets["mouse_aim_sens"] = Segmented(aim, MOUSE_AIM_FACTORS, settings.get("mouse_aim_sens", "1.0"),
                                                   lambda v: self.change_game_setting("mouse_aim_sens", v),
                                                   self.f_small)
        self.widgets["mouse_aim_sens"].pack(side="left", padx=(self.px(8), self.px(22)))
        tk.Label(aim, text="Up/down", font=self.f_body_bold, bg=BG, fg=TEXT).pack(side="left")
        self.widgets["mouse_vertical"] = Segmented(aim, MOUSE_FACTORS, settings.get("mouse_vertical", "1.0"),
                                                   lambda v: self.change_game_setting("mouse_vertical", v),
                                                   self.f_small)
        self.widgets["mouse_vertical"].pack(side="left", padx=(self.px(8), self.px(22)))
        tk.Label(aim, text="Invert X", font=self.f_body_bold, bg=BG, fg=TEXT).pack(side="left")
        self.widgets["mouse_invert_x"] = Toggle(aim, self.s, settings.get("mouse_invert_x", False),
                                                lambda v: self.change_game_setting("mouse_invert_x", v))
        self.widgets["mouse_invert_x"].pack(side="left", padx=(self.px(8), 0))
        # Controller vibration (#166): off stops every rumble.
        rumble = tk.Frame(page, bg=BG)
        rumble.pack(side="top", fill="x", pady=(self.px(10), 0))
        tk.Label(rumble, text="Controller vibration", font=self.f_body_bold, bg=BG, fg=TEXT).pack(side="left")
        self.widgets["vibration"] = Toggle(rumble, self.s, settings.get("vibration", True),
                                           lambda v: self.change_game_setting("vibration", v))
        self.widgets["vibration"].pack(side="left", padx=(self.px(8), 0))
        icons = tk.Frame(page, bg=BG)
        icons.pack(side="top", fill="x", pady=(self.px(10), 0))
        tk.Label(icons, text="Button icons", font=self.f_body_bold, bg=BG, fg=TEXT).pack(side="left")
        self.widgets["prompts"] = Segmented(icons, PROMPT_MODES, settings.get("prompts", "auto"),
                                            self.change_prompts, self.f_small)
        self.widgets["prompts"].pack(side="left", padx=(self.px(8), self.px(12)))
        tk.Label(icons, text="Auto follows the device you play with (a controller's own buttons).",
                 font=self.f_small, bg=BG,
                 fg=MUTED).pack(side="left")
        self.controls_hint = self.wrap(tk.Label(
            page, text="Click a slot, then press a key or a mouse button (right-click a slot to clear it). "
                       "In game the mouse is captured; Alt+Tab or Insert (menu) frees it. A controller "
                       "always works too.", font=self.f_small, bg=BG, fg=MUTED, anchor="w", justify="left",
            wraplength=self.px(800)))
        self.controls_hint.pack(side="top", fill="x", pady=(self.px(6), self.px(4)))
        holder = tk.Frame(page, bg=BG)
        holder.pack(side="top", fill="both", expand=True)
        canvas = tk.Canvas(holder, bg=BG, highlightthickness=0)
        bar = ScrollBar(holder, self.s, canvas)
        inner = tk.Frame(canvas, bg=BG)
        inner.bind("<Configure>", lambda e: canvas.configure(scrollregion=canvas.bbox("all")))
        canvas.create_window((0, 0), window=inner, anchor="nw")
        canvas.configure(yscrollcommand=bar.set)
        bar.pack(side="right", fill="y")
        canvas.pack(side="left", fill="both", expand=True)

        self.scroll_canvases.add(str(canvas))
        self.capturing = None
        self.binding_slots = {}
        self.pad_slots = {}
        self.pad_capture = None
        if not hasattr(self, "gamepads"):
            self.gamepads = Gamepads()
        columns = [tk.Frame(inner, bg=BG), tk.Frame(inner, bg=BG)]
        columns[0].grid(row=0, column=0, sticky="n", padx=(0, self.px(36)))
        columns[1].grid(row=0, column=1, sticky="n")
        for index, (group, actions) in enumerate(CONTROL_GROUPS):
            column = columns[0 if index < 2 else 1]
            tk.Label(column, text=group, font=self.f_small, bg=BG, fg=ACCENT, anchor="w").pack(
                side="top", fill="x", pady=(self.px(6 if index % 2 else 2), self.px(2)))
            for action, label in actions:
                row = tk.Frame(column, bg=BG)
                row.pack(side="top", fill="x", pady=self.px(1))
                tk.Label(row, text=label, font=self.f_small, bg=BG, fg=TEXT, anchor="w",
                         width=17).pack(side="left")
                for slot in (0, 1):
                    cell = tk.Label(row, text="", font=self.f_small, bg=PANEL, fg=TEXT, width=14,
                                    pady=self.px(3), cursor="hand2", highlightthickness=2,
                                    highlightbackground=LINE, highlightcolor=ACCENT, takefocus=1)
                    cell.pack(side="left", padx=(self.px(6), 0))
                    cell.bind("<Button-1>", lambda e, a=action, i=slot: self.start_capture(a, i))
                    cell.bind("<Button-3>", lambda e, a=action, i=slot: self.set_binding(a, i, None))
                    # Keyboard: Enter/Space is a click, Delete is a right-click.
                    on_activate(cell, lambda c=cell, a=action, i=slot: self.key_capture(c, a, i))
                    on_keys(cell, {"<Delete>": lambda a=action, i=slot: self.set_binding(a, i, None),
                                   "<BackSpace>": lambda a=action, i=slot: self.set_binding(a, i, None)})
                    self.binding_slots[(action, slot)] = cell
                # The controller button: its icon; a click, then a controller button rebinds it.
                pad_cell = tk.Label(row, text="", font=self.f_small, bg=PANEL, fg=TEXT, width=14, height=1,
                                    highlightthickness=2, highlightbackground=LINE, highlightcolor=ACCENT,
                                    compound="center")
                pad_cell.pack(side="left", padx=(self.px(6), 0))
                if action in PAD_DEFAULTS:
                    pad_cell.config(cursor="hand2", takefocus=1)
                    pad_cell.bind("<Button-1>", lambda e, a=action: self.start_pad_capture(a))
                    pad_cell.bind("<Button-3>", lambda e, a=action: self.set_pad_binding(a, None))
                    on_activate(pad_cell, lambda a=action: self.key_capture(None, a))
                    on_keys(pad_cell, {"<Delete>": lambda a=action: self.set_pad_binding(a, None),
                                       "<BackSpace>": lambda a=action: self.set_pad_binding(a, None)})
                self.pad_slots[action] = pad_cell
        self.refresh_bindings()
        self.refresh_pad_slots()
        return page

    def game_controls(self):
        return merged_controls(self.game_settings().get("controls"))

    def change_prompts(self, value):
        self.change_game_setting("prompts", value)
        self.refresh_pad_slots()

    def pad_style(self):
        style = self.game_settings().get("prompts", "auto")
        if style in ("xbox", "playstation"):
            return style
        return self.gamepads.style() or "xbox"  # Auto: the connected controller's

    def refresh_pad_slots(self):
        style = self.pad_style()
        controls = merged_pad_controls(self.game_settings().get("pad_controls"))
        if not hasattr(self, "pad_images"):
            self.pad_images = {}
        height = self.px(22)
        for action, cell in getattr(self, "pad_slots", {}).items():
            if self.pad_capture == action:
                cell.config(image="", text="Press a button…", fg=ACCENT, bg=PANEL_HI, width=14, height=1)
                continue
            button = controls.get(action) if action in PAD_DEFAULTS else PAD_FIXED.get(action)
            if not button:
                cell.config(image="", text="—", fg=TEXT, bg=PANEL, width=14, height=1)
                continue
            key = (button, style, height)
            if key not in self.pad_images:
                self.pad_images[key] = ImageTk.PhotoImage(pad_icon(button, style, height))
            cell.config(image=self.pad_images[key], text="", bg=PANEL, width=self.px(100), height=self.px(24))

    def start_pad_capture(self, action):
        self.stop_capture()
        if not self.gamepads.sdl:
            self.controls_hint.config(text="No controller support found (SDL3.dll).", fg=STOP_RED)
            return
        self.pad_capture = action
        self.pad_capture_started = time.monotonic()
        self.pad_capture_held = self.gamepads.held()  # buttons already down do not count
        if not self.gamepads.pads:
            self.controls_hint.config(text="Connect a controller, then click the slot again.", fg=STOP_RED)
            self.pad_capture = None
            return
        self.controls_hint.config(text="Press a button on the controller (Escape cancels). Triggers and sticks "
                                       "stay Aim, Fire, Move and Look.", fg=MUTED)
        self.refresh_pad_slots()
        self.root.bind_all("<Escape>", lambda e: self.stop_pad_capture())
        self.root.after(30, self.poll_pad_capture)

    def poll_pad_capture(self):
        if self.pad_capture is None:
            return
        held = self.gamepads.held()
        new = [b for b in SDL_BUTTONS if b in held and b not in self.pad_capture_held]
        self.pad_capture_held &= held
        if any(t in held and t not in self.pad_capture_held for t in ("lefttrigger", "righttrigger")):
            self.controls_hint.config(text="The triggers stay Aim and Fire. Press a button.", fg=STOP_RED)
        if new:
            self.set_pad_binding(self.pad_capture, new[0])
            return
        if time.monotonic() - self.pad_capture_started > 8:
            self.stop_pad_capture()
            return
        self.root.after(30, self.poll_pad_capture)

    def stop_pad_capture(self):
        self.pad_capture = None
        self.root.unbind_all("<Escape>")
        self.refresh_pad_slots()

    def set_pad_binding(self, action, button):
        settings = self.game_settings()
        pad_controls = dict(settings.get("pad_controls") or {})
        merged = merged_pad_controls(pad_controls)
        if button:
            # A button does one thing: the action that had it swaps to this one's old button.
            for other, current in merged.items():
                if current == button and other != action:
                    pad_controls[other] = merged[action]
        pad_controls[action] = button or ""
        settings["pad_controls"] = {a: b for a, b in merged_pad_controls(pad_controls).items() if b != PAD_DEFAULTS.get(a)}
        self.save_controls()
        self.pad_capture = None
        self.root.unbind_all("<Escape>")
        self.refresh_pad_slots()
        self.controls_hint.config(text="Saved for " + self.current_game()["name"] + ". Right-click a controller "
                                       "slot to clear it; Reset controls restores the defaults.", fg=MUTED)

    def refresh_bindings(self):
        controls = self.game_controls()
        for (action, slot), cell in self.binding_slots.items():
            bindings = controls.get(action, [])
            capturing = self.capturing == (action, slot)
            cell.config(text="Press a key…" if capturing else binding_label(bindings[slot] if slot < len(bindings) else None),
                        bg=PANEL_HI if capturing else PANEL, fg=ACCENT if capturing else TEXT)

    def key_capture(self, cell, action, slot=None):
        """Enter/Space on a focused Controls slot (slot None: the controller button) starts the
        capture like a click; a key capture takes the focus (start_capture), so it returns to the
        slot when the capture ends."""
        if self.capturing is not None or self.pad_capture is not None:
            return
        if slot is None:
            self.start_pad_capture(action)
        else:
            self.capture_back = cell
            self.start_capture(action, slot)

    def restore_focus(self):
        cell, self.capture_back = self.capture_back, None
        if cell is not None and cell.winfo_exists():
            cell.focus_set()

    def start_capture(self, action, slot):
        self.stop_capture()
        self.capturing = (action, slot)
        self.capture_armed = False
        self.refresh_bindings()
        self.root.focus_set()
        # After this click: the next key, mouse button or wheel step is the binding.
        self.root.after(200, self.arm_capture)

    def arm_capture(self):
        if self.capturing is None:
            return
        self.capture_armed = True

    def stop_capture(self):
        if getattr(self, "capturing", None) is None:
            return
        self.capturing = None
        self.capture_armed = False
        self.refresh_bindings()
        self.restore_focus()

    def capture_key(self, event):
        keysym = event.keysym
        if keysym in RESERVED_KEYS:
            self.controls_hint.config(text=f"{keysym} is used by the game window (Insert opens the menu, F9 "
                                           "records input, comma separates bindings). Choose another key.",
                                      fg=STOP_RED)
            self.stop_capture()
            return "break"
        name = TK_TO_SDL.get(keysym) or (keysym.upper() if len(keysym) == 1 and keysym.isalnum() else None)
        if not name:
            self.controls_hint.config(text=f"The key {keysym} can't be used. Choose another key.", fg=STOP_RED)
            self.stop_capture()
            return "break"
        action, slot = self.capturing
        self.set_binding(action, slot, f"key:{name}")
        return "break"

    def capture_button(self, event):
        # Tk 9 (the bundled Python 3.14) numbers the side buttons 8 and 9 on Windows (#152); 4 and
        # 5 were Tk 8's numbers.
        names = {1: "left", 2: "middle", 3: "right", 4: "x1", 5: "x2", 8: "x1", 9: "x2"}
        if event.num in names:
            self.capture_mouse(names[event.num])
        return "break"

    def capture_wheel(self, event):
        self.capture_mouse("wheel_up" if event.delta > 0 else "wheel_down")
        return "break"

    def capture_mouse(self, name):
        action, slot = self.capturing
        if action in STICK_ACTIONS:
            self.controls_hint.config(text="Stick directions take keys; the mouse moves the camera by itself.",
                                      fg=STOP_RED)
            self.stop_capture()
            return
        self.set_binding(action, slot, f"mouse:{name}")

    def set_binding(self, action, slot, binding):
        controls = self.game_controls()
        if binding:
            # A key or button does one thing: it leaves the action it had.
            for other, bindings in controls.items():
                if binding in bindings and (other, bindings.index(binding)) != (action, slot):
                    bindings.remove(binding)
        bindings = controls[action]
        while len(bindings) <= slot:
            bindings.append(None)
        bindings[slot] = binding
        controls[action] = [b for b in bindings if b][:2]
        self.game_settings()["controls"] = controls
        self.save_controls()
        self.controls_hint.config(
            text="Saved for " + self.current_game()["name"] + ". Click a slot, then press a key or a mouse button "
                 "(right-click a slot to clear it).", fg=MUTED)
        self.capturing = None
        self.capture_armed = False
        self.refresh_bindings()
        self.restore_focus()

    def reset_controls(self):
        name = self.current_game()["name"]
        if not self.ask_yesno(f"Reset {name}'s keyboard, mouse and controller controls?"):
            return
        settings = self.game_settings()
        settings.update({"controls": None, "mouse": True, "mouse_sens": "1.0", "mouse_invert": False,
                         "prompts": "auto", "pad_controls": None, "mouse_aim_sens": "1.0",
                         "mouse_vertical": "1.0", "mouse_invert_x": False, "vibration": True})
        self.save_controls()
        self.refresh_controls()

    def page_cheats(self):
        page = self.page()
        tk.Label(page, text="The games' own developer switches, applied when a game starts. They need update 1.02.",
                 font=self.f_small, bg=BG, fg=MUTED, anchor="w").pack(side="top", fill="x", pady=(0, self.px(6)))
        for name, title, hint in CHEATS:
            def cheat(parent, key="cheat_" + name):
                return Toggle(parent, self.s, self.config.get(key, False), lambda v, k=key: self.change_config(k, v))
            self.row(page, title, hint, cheat, badge=ALL_GAMES)
        return page

    def page_advanced(self):
        page = self.page()

        # Save profiles (#66): profile N plays as PS4 user N, with its saves in savedata\N.
        def save_profile(parent):
            return Segmented(parent, (("1", "Profile 1"), ("2", "Profile 2"), ("3", "Profile 3")),
                             str(self.game_settings().get("save_profile", "1")),
                             lambda v: self.change_game_setting("save_profile", v), self.f_small)
        self.widgets["save_profile"] = self.row(
            page, "Save profile",
            "Separate saves for this game: each profile keeps its own progress and in-game settings, "
            "like another PS4 user. Profile 1 has your saves from earlier versions; switching never "
            "deletes anything.", save_profile,
            short="Separate saves for this game, like another PS4 user. Switching never deletes anything.")

        def language(parent):
            voices = voice_languages(self.game_dir())
            options = [(code, label + ("  (+ voices)" if code in voices else "")) for code, label in LANGUAGES]
            return Dropdown(parent, options, self.config.get("language", "1"),
                            lambda v: self.change_config("language", v), self.f_small)
        self.widgets["language"] = self.row(
            page, "Language",
            "Menus and subtitles in another language: pick it here, then press Square in a game's main "
            "menu to switch. Languages marked + voices are also spoken in your copy of the game.", language,
            short="Pick a language here, then press Square in a game's main menu to switch.", badge=ALL_GAMES)

        def speakers(parent):
            return Segmented(parent, (("auto", "Auto"), ("2", "Stereo"), ("6", "5.1"), ("8", "7.1")),
                             str(self.config.get("speakers", "auto")),
                             lambda v: self.change_config("speakers", v), self.f_small, recommended="auto")
        self.row(page, "Speakers",
                 "The games have their own stereo, 5.1 and 7.1 sound mixes. Auto picks the one that matches "
                 "your Windows sound output (Sound settings -> speaker setup); headphones get stereo.",
                 speakers, short="Auto picks the stereo, 5.1 or 7.1 mix that matches your Windows sound output.",
                 badge=ALL_GAMES)

        def unlock(parent):
            return Toggle(parent, self.s, self.config.get("unlock", False),
                          lambda v: self.change_config("unlock", v))
        self.row(page, "Unlock all chapters",
                 "Every chapter in Chapter Select and the Brutal difficulty from the start, in all three "
                 "games. Your saves and progress stay as they are. Needs update 1.02.", unlock,
                 short="Every chapter and the Brutal difficulty from the start. Needs update 1.02.", badge=ALL_GAMES)

        def unlock_rewards(parent):
            return Toggle(parent, self.s, self.config.get("unlock_rewards", False),
                          lambda v: self.change_config("unlock_rewards", v))
        self.row(page, "Unlock all rewards",
                 "Every reward in Bonuses (skins, weapons, render modes, art galleries, gameplay tweaks) "
                 "and every story cutscene, while this is on. Nothing is saved: turned off, the menus show "
                 "your own progress again. Needs update 1.02.", unlock_rewards,
                 short="Every Bonuses reward and story cutscene, while this is on. Needs update 1.02.",
                 badge=ALL_GAMES)

        def diagnostics(parent):
            return Toggle(parent, self.s, self.config.get("diagnostics", True),
                          lambda v: self.change_config("diagnostics", v))
        self.row(page, "Performance log", "Writes frame rate statistics to the log every 5 seconds. Helps when "
                                          "you report a problem.", diagnostics, badge=ALL_GAMES)

        def torch_forward(parent):
            return Toggle(parent, self.s, self.config.get("torch_forward", True),
                          lambda v: self.change_config("torch_forward", v))
        self.row(page, "Torch follows Nate (Uncharted 2)",
                 "The torch lights where Nate points it. Off: as on the PS4, it lights where the camera "
                 "looks. Needs update 1.02.", torch_forward,
                 short="The torch lights where Nate points it; off, where the camera looks. Needs update 1.02.",
                 badge=ALL_GAMES)

        def check_updates(parent):
            return Toggle(parent, self.s, self.config.get("check_updates", True),
                          lambda v: self.change_config("check_updates", v))
        version = updater.installed_version(ROOT)
        self.row(page, "Check for updates at start",
                 f"Installed: {updater.version_text(version) if version else 'source tree (updates with git)'}. "
                 "The launcher asks before it installs anything.", check_updates, badge=ALL_GAMES)

        def section(title, actions, note=""):
            tk.Label(page, text=title, font=self.f_small, bg=BG, fg=ACCENT, anchor="w").pack(
                side="top", fill="x", pady=(self.px(10), self.px(4)))
            line = tk.Frame(page, bg=BG)
            line.pack(side="top", fill="x")
            for label, action in actions:
                self.flat_button(line, label, action).pack(side="left", padx=(0, self.px(10)))
            if note:
                tk.Label(line, text=note, font=self.f_small, bg=BG, fg=MUTED, anchor="w").pack(
                    side="left", padx=(self.px(6), 0))
        section("SHORTCUTS", (("Desktop shortcut", self.desktop_shortcut), ("Game shortcuts", self.game_shortcuts)),
                "Game shortcuts: one per game on the desktop, each starts its game directly.")
        section("FOLDERS", (("Open log folder", self.open_logs), ("Open save folder", self.open_saves),
                            ("Change game folder", self.choose_game_dir),
                            ("Install game from .pkg", self.install_game_pkg)))
        self.game_dir_label = tk.Label(page, text=f"Game files: {self.game_dir()}", font=self.f_small,
                                       bg=BG, fg=DIM, anchor="w")
        self.game_dir_label.pack(side="top", fill="x", pady=(self.px(6), 0))
        self.update_label = self.wrap(tk.Label(page, text=self.update_text(), font=self.f_small, bg=BG, fg=DIM,
                                               anchor="w", justify="left", wraplength=self.px(800)))
        self.update_label.pack(side="top", fill="x")
        self.ini_label = tk.Label(page, text=f"Settings: {ini_file(self.config['game'])}", font=self.f_small,
                                  bg=BG, fg=DIM, anchor="w")
        self.ini_label.pack(side="top", fill="x")
        section("GAME UPDATE", (("Choose update folder", self.choose_update_folder),
                                ("Install from .pkg", self.install_game_update)),
                "Your extracted update 1.02 folder, wherever it is, or the update's .pkg file.")
        section("TROUBLESHOOTING", (("Check for updates", lambda: self.check_updates(manual=True)),
                                    ("Clear shader cache", self.clear_cache), ("Reset this game's settings", self.reset)))
        # Credits for the anti-aliasing technologies (NVIDIA's DLSS license asks for its attribution).
        self.wrap(tk.Label(page, text=CREDITS, font=self.f_small, bg=BG, fg=DIM, anchor="w", justify="left",
                           wraplength=self.px(800))).pack(side="top", fill="x", pady=(self.px(10), 0))
        return page

    def flat_button(self, parent, label, action):
        button = tk.Label(parent, text=label, font=self.f_small, bg=PANEL, fg=TEXT, padx=self.px(12), pady=self.px(6),
                          cursor="hand2", highlightthickness=2, highlightbackground=LINE, highlightcolor=ACCENT,
                          takefocus=1)
        button.bind("<Button-1>", lambda e: action())
        on_activate(button, action)
        button.bind("<Enter>", lambda e: button.config(bg=PANEL_HI))
        button.bind("<Leave>", lambda e: button.config(bg=PANEL))
        return button

    # -- footer -----------------------------------------------------------------------------

    def build_footer(self):
        footer = self.footer = tk.Frame(self.main, bg=PANEL)
        footer.pack(side="bottom", fill="x")
        tk.Frame(self.main, bg=LINE, height=1).pack(side="bottom", fill="x")
        inner = tk.Frame(footer, bg=PANEL)
        inner.pack(fill="x", padx=self.px(28), pady=self.px(12))
        left = tk.Frame(inner, bg=PANEL)
        self.status = tk.Label(left, text="", font=self.f_body_bold, bg=PANEL, fg=TEXT, anchor="w")
        self.status.pack(side="top", fill="x")
        self.substatus = tk.Label(left, text="Insert or R3 + L2 in game opens the menu.", font=self.f_small,
                                  bg=PANEL, fg=MUTED, anchor="w")
        self.substatus.pack(side="top", fill="x")
        w, h = self.px(210), self.px(54)
        self.play_button = tk.Canvas(inner, width=w, height=h, bg=PANEL, highlightthickness=2,
                                     highlightbackground=PANEL, highlightcolor=ACCENT, takefocus=1, cursor="hand2")
        self.play_button.pack(side="right")
        self.play_button.bind("<Button-1>", lambda e: self.play_or_stop())
        on_activate(self.play_button, self.play_or_stop)
        self.play_button.bind("<Enter>", lambda e: self.draw_play(hover=True))
        self.play_button.bind("<Leave>", lambda e: self.draw_play())
        self.draw_play()
        # After PLAY: a long status line is cut, never the button.
        left.pack(side="left", fill="x", expand=True)
        self.update_button = tk.Label(inner, text="UPDATE", font=self.f_body_bold, bg=PANEL_HI, fg=ACCENT,
                                      padx=self.px(16), pady=self.px(10), cursor="hand2", takefocus=1,
                                      highlightthickness=2, highlightbackground=ACCENT, highlightcolor=TEXT)  # TEXT: the ring must differ from its own orange border
        self.update_button.bind("<Button-1>", lambda e: self.install_update())
        on_activate(self.update_button, self.install_update)
        self.set_status(f"Ready to play {self.current_game()['name']}." if self.config.get("game") else "Ready.")

    def draw_play(self, hover=False):
        canvas = self.play_button
        w, h = int(canvas["width"]), int(canvas["height"])
        canvas.delete("all")
        running = self.process is not None
        fill = (STOP_RED if running else (ACCENT_HI if hover else ACCENT))
        round_rect(canvas, 2, 2, w - 2, h - 2, self.px(8), fill=fill, outline=fill)
        canvas.create_text(w // 2, h // 2, text="STOP" if running else "PLAY", font=self.f_play,
                           fill=TEXT if running else INK)

    def set_status(self, text, detail=None):
        self.status.config(text=text)
        if detail is not None:
            self.substatus.config(text=detail)

    # -- messages (24 Oct 2026: themed Dialog/Toast in place of the native message boxes) ----

    def info(self, text, detail=None):
        Dialog.info(self.root, APP_TITLE, text, detail, scale=self.s)

    def error(self, text, detail=None):
        Dialog.error(self.root, APP_TITLE, text, detail, scale=self.s)

    def warn(self, text):
        Dialog.warning(self.root, APP_TITLE, text, scale=self.s)

    def ask_yesno(self, text):
        return Dialog.yesno(self.root, APP_TITLE, text, scale=self.s)

    def ask_okcancel(self, text):
        return Dialog.okcancel(self.root, APP_TITLE, text, scale=self.s)

    def toast(self, text, kind="info"):
        Toast.show(self.root, text, kind, self.s)

    # -- updates (launcher/updater.py) ------------------------------------------------------

    def check_updates(self, manual=False):
        current = updater.installed_version(ROOT)
        if current is None:
            if manual:
                self.info("This copy runs from the source tree: update it with git.")
            return
        if self.update_busy:
            return
        self.update_busy = True
        if manual:
            self.set_status("Checking for updates...", "")

        def work():
            try:
                self.update_result = (updater.latest_release(current), None)
            except Exception as error:  # offline, GitHub's rate limit
                self.update_result = (None, str(error) or error.__class__.__name__)
        threading.Thread(target=work, daemon=True).start()
        self.root.after(200, lambda: self.update_checked(manual))

    def update_checked(self, manual):
        if self.update_result is None:
            self.root.after(200, lambda: self.update_checked(manual))
            return
        release, error = self.update_result
        self.update_result = None
        self.update_busy = False
        if release:
            self.update_release = release
            version = updater.version_text(release["version"])
            self.update_button.config(text=f"UPDATE TO {version}")
            self.update_button.pack(side="right", padx=(0, self.px(14)), before=self.status.master)
            self.refresh_home()
            if not self.process:
                self.set_status(f"Version {version} is available.",
                                "UPDATE installs it: saves and settings are kept, the launcher restarts.")
        elif manual:
            current = updater.version_text(updater.installed_version(ROOT))
            if error:
                self.set_status("Could not check for updates.", error)
            else:
                self.set_status("No update found.", f"Version {current} is the newest.")

    def install_update(self):
        release = self.update_release
        if not release or self.update_busy:
            return
        if self.process:
            self.info("Close the game first: the update replaces the game's runtime.")
            return
        version = updater.version_text(release["version"])
        if not self.ask_yesno(f"Install version {version} ({release['size'] / 1e6:.0f} MB "
                              "download)?\n\nSaves, settings and the shader cache are kept. The "
                              "launcher closes, installs the update and starts again.\n\n"
                              f"What's new: {release['page']}"):
            return
        self.update_busy = True
        self.update_progress = (0, release["size"])
        self.update_error = None
        self.update_staged = None

        def progress(done, total):
            self.update_progress = (done, total)

        def work():
            try:
                archive = updater.download(release, ROOT, progress)
                self.update_staged = updater.stage(archive, ROOT, release["version"])
            except Exception as error:
                self.update_error = str(error) or error.__class__.__name__
        threading.Thread(target=work, daemon=True).start()
        self.root.after(200, self.update_tick)

    def update_tick(self):
        if self.update_error:
            self.update_busy = False
            self.set_status("The update failed.", "The installed version is unchanged.")
            self.error("The update could not be installed:", self.update_error)
            return
        if self.update_staged:
            self.set_status("Installing the update...", "The launcher starts again in a moment.")
            try:
                updater.start_apply(ROOT, self.update_staged)
            except OSError as error:
                self.update_busy = False
                self.error("The update could not be started:", str(error))
                return
            self.config["window_xy"] = [self.root.winfo_x(), self.root.winfo_y()]
            write_config(self.config)
            self.root.destroy()
            return
        done, total = self.update_progress
        amount = f"{100 * done / total:.0f}%" if total else f"{done / 1e6:.0f} MB"
        self.set_status(f"Downloading the update... {amount}", "Keep the launcher open.")
        self.root.after(200, self.update_tick)

    # -- settings changes -------------------------------------------------------------------

    def note_next_start(self):
        """24 Oct 2026: a setting the in-game menu cannot change is read at the start: say so once per run."""
        if self.process and not self.next_start_noted:
            self.next_start_noted = True
            self.toast("Applies the next time the game starts. Insert opens the in-game menu for live changes.",
                       "info")

    def apply_to_all_games(self):
        """Copies the selected game's Display and Performance settings (its ini keys, resolution, aspect
        ratio, multithreading and quality preset) to the other two games; controls stay as they are."""
        if self.process:
            self.warn("Close the game first: the Collection may be running any of the three games.")
            return
        name = self.current_game()["name"]
        if not self.ask_yesno(f"Copy {name}'s Display and Performance settings (resolution, aspect ratio, "
                              "anti-aliasing, frame rate and the rest) to the other two games? Their "
                              "controls stay as they are."):
            return
        source = self.config["game"]
        shared = {k: self.ini.get(k, v) for k, v in INI_DEFAULTS.items()}
        mine = self.game_settings()
        for other in GAME_KEYS:
            if other == source:
                continue
            write_ini(other, shared)
            settings = self.config["games"][other]
            for key in ("resolution", "aspect", "multithreaded", "preset"):
                if key in mine:
                    settings[key] = mine[key]
        write_config(self.config)
        self.toast("Settings copied to the other games.", "ok")

    def change(self, key, value):
        if key == "gpu_id":
            self.note_next_start()
        self.ini[key] = value
        self.save_ini({key: value})
        if self.game_settings().get("preset") != "custom":
            self.game_settings()["preset"] = "custom"
            write_config(self.config)
            self.redraw_presets()
        if key == "fps_limit":
            self.update_fps_note()
        if key == "upscaler":
            self.refresh_dependencies()

    def change_resolution(self, value):
        self.note_next_start()
        self.game_settings()["resolution"] = value
        self.game_settings()["preset"] = "custom"
        write_config(self.config)
        self.redraw_presets()
        self.refresh_render_size()

    def change_aspect(self, value):
        self.change_game_setting("aspect", value)
        self.refresh_render_size()

    def refresh_render_size(self):
        """The resolution labels and the exact render size for the selected game's aspect ratio."""
        settings = self.game_settings()
        aspect = settings.get("aspect", "16:9")
        if isinstance(self.widgets.get("resolution"), Segmented):
            self.widgets["resolution"].relabel(resolution_options(aspect))
        if hasattr(self, "render_size_note"):
            width, height = aspect_resolution(settings["resolution"], aspect).split("x")
            self.render_size_note.config(text=f"Render size: {width} × {height}")

    def change_game_setting(self, key, value):
        if key in ("aspect", "multithreaded"):
            self.note_next_start()
        self.game_settings()[key] = value
        if key in ("mouse", "mouse_sens", "mouse_invert", "prompts", "mouse_aim_sens", "mouse_vertical",
                   "mouse_invert_x", "vibration"):
            self.save_controls()
        else:
            write_config(self.config)

    def change_config(self, key, value):
        self.config[key] = value
        write_config(self.config)

    # -- game -------------------------------------------------------------------------------

    def play_or_stop(self):
        if self.process:  # Space/Enter/A on the focused button must not kill the game unasked
            if self.ask_yesno("The game is still running. Stop it? Unsaved progress is lost."):
                self.stop()
        else:
            self.play()

    def play_game(self, key):
        """A double-click on a game's card: choose it and play."""
        self.select_game(key)
        if not self.process:
            self.play()

    def play(self):
        if not RUN_SCRIPT.exists():
            self.error(f"Start script missing:\n{RUN_SCRIPT}")
            return
        if not is_game_dir(self.game_dir()):
            self.info("Select your Uncharted: The Nathan Drake Collection game folder "
                      "(CUSA02320 or CUSA02344, the folder with eboot.bin and sce_sys).")
            if not self.choose_game_dir():
                return
        if not self.confirm_without_update() or not self.confirm_update_files() or \
                not self.confirm_slow_drive():
            return
        self.follow_ini()  # the in-game menu's last changes, not older values kept here
        self.save_ini({k: self.ini.get(k, v) for k, v in INI_DEFAULTS.items()})
        write_config(self.config)
        game = self.current_game()
        env = os.environ.copy()
        env["UNCHARTED_DIAGNOSTICS"] = "1" if self.config.get("diagnostics", True) else "0"
        env["UNCHARTED_TORCH_FORWARD"] = "1" if self.config.get("torch_forward", True) else "0"
        env["UNCHARTED_JOB_SLEEP"] = "0" if self.config.get("job_sleep", "auto") == "off" else "1"
        env["UNCHARTED_LANGUAGE"] = str(self.config.get("language", "1"))
        env["UNCHARTED_SPEAKERS"] = str(self.config.get("speakers", "auto"))
        env["UNCHARTED_UNLOCK"] = "1" if self.config.get("unlock", False) else "0"
        env["UNCHARTED_UNLOCK_REWARDS"] = "1" if self.config.get("unlock_rewards", False) else "0"
        if self.chosen_update():
            env["UNCHARTED_UPDATE"] = str(self.chosen_update())  # tools/probe_uncharted.py find_update
        env["UNCHARTED_CHEATS"] = ",".join(name for name, *_ in CHEATS if self.config.get("cheat_" + name, False))
        # Every game's own options: the Collection menu starts the others itself, and
        # tools/probe_uncharted.py applies the started game's (its settings file decides the
        # vblank rate for frame rates above 60).
        env.pop("GOW3_VBLANK_HZ", None)
        for key in GAME_KEYS:
            settings = self.config["games"][key]
            env[f"UNCHARTED_RESOLUTION_{key.upper()}"] = aspect_resolution(settings["resolution"],
                                                                         settings.get("aspect", "16:9"))
            env[f"UNCHARTED_MULTITHREADED_{key.upper()}"] = "1" if settings["multithreaded"] else "0"
            env[f"UNCHARTED_SAVE_PROFILE_{key.upper()}"] = str(settings.get("save_profile", "1"))
            # Controls: in each game's settings file (save_controls), where its menu changes them.
        python = Path(sys.executable)
        if python.name.lower() == "pythonw.exe" and python.with_name("python.exe").exists():
            python = python.with_name("python.exe")
        command = [str(python), "-u", str(RUN_SCRIPT), "--boot", "--seconds", "0", "--executable", game["exe"],
                   "--game", str(self.game_dir())]
        # The game window may take the foreground (Windows only lets the foreground process do
        # that; the game starts two processes down): fullscreen and keyboard input need the focus.
        try:
            ctypes.windll.user32.AllowSetForegroundWindow(-1)  # ASFW_ANY
        except (AttributeError, OSError):
            pass
        try:
            self.process = subprocess.Popen(command, cwd=str(ROOT), env=env, stdout=subprocess.PIPE,
                                            stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                                            creationflags=NO_WINDOW, text=True, encoding="utf-8", errors="replace")
        except OSError as error:
            self.error("Could not start the game:", str(error))
            self.process = None
            return
        self.started_at = time.time()
        self.last_output = time.time()
        self.game_started = False
        # The start script's output (preparation, patches, the runtime's log path): kept for
        # "the game does not start" reports.
        try:
            self.run_log = open(USER_DIR / "launcher-last-run.log", "w", encoding="utf-8")
            self.run_log.write(f"{time.strftime('%Y-%m-%d %H:%M:%S')} {' '.join(command)}\n")
        except OSError:
            self.run_log = None
        threading.Thread(target=self.read_output, args=(self.process,), daemon=True).start()
        self.set_status(f"Starting {game['name']}...", "Preparing game files and code patches.")
        self.draw_play()

    def read_output(self, process):
        log = getattr(self, "run_log", None)
        for line in process.stdout:
            self.output.put(line.rstrip())
            if log:
                try:
                    log.write(line)
                    log.flush()
                except (OSError, ValueError):
                    log = None
        if log:
            try:
                log.write(f"exit code {process.wait()}\n")
                log.close()
            except (OSError, ValueError):
                pass

    def poll(self):
        while True:
            try:
                line = self.output.get_nowait()
            except queue.Empty:
                break
            self.last_output = time.time()
            if line.startswith("Collection: switching to"):
                target = line.split("switching to", 1)[1].strip().split()[0]
                name = next((g["name"] for g in GAMES if g["exe"] == target), target)
                self.set_status(f"Switching to {name}...", "The Collection is starting another game.")
                self.game_started = False
        self.poll_count = getattr(self, "poll_count", 0) + 1
        if self.poll_count % 7 == 0:  # about once a second
            self.follow_ini()
        if self.process:
            if self.process.poll() is not None:
                self.finished(self.process.returncode)
            elif not self.game_started and time.time() - self.last_output > 2.5:
                self.game_started = True
                self.set_status(f"Playing {self.current_game()['name']}",
                                "Insert or R3 + L2 opens the in-game menu. STOP closes the game.")
        self.root.after(150, self.poll)

    def latest_log(self):
        logs = [ROOT / "analysis" / g["log"] / "boot.log" for g in GAMES]
        logs = [log for log in logs if log.exists()]
        return max(logs, key=lambda p: p.stat().st_mtime) if logs else None

    def crash_reason(self):
        log = self.latest_log()
        if not log or log.stat().st_mtime < self.started_at:
            return None
        try:
            tail = log.read_text(encoding="utf-8", errors="replace").splitlines()[-80:]
        except OSError:
            return None
        for line in reversed(tail):
            if line.startswith("Hang watchdog:") and " report " in line:
                return ("The game froze. A report of where it stopped was saved:\n"
                        + line.split(" report ", 1)[-1] + "\n\nPlease attach it with the log folder's "
                        "boot.log to a GitHub issue.")
        # A crash (exit code 3): the runtime's fault report names where it happened. Players
        # reported just "error code 3" (GitHub #24, #27); the report is what tells the cause.
        for i, line in enumerate(tail):
            if line.startswith("Fault:"):
                where = next((l.strip() for l in tail[i + 1:i + 6] if l.lstrip().startswith(("host #0", "#0"))), "")
                return (line.strip() + (f"\n{where}" if where else "") + "\n\nPlease attach boot.log from the "
                        "log folder (and your graphics card, game version and settings) to a GitHub issue.")
        for i, line in enumerate(tail):
            if line.startswith("STOP:") or "Unreachable code" in line or "Assertion" in line:
                context = [l for l in tail[max(0, i - 2):i + 1] if l.strip()]
                return "\n".join(context)[-600:]
        return None

    def confirm_update_files(self):
        """An update folder with missing or damaged files (#43: a bin.psarc the games couldn't read
        stopped all three at start with "ASSERTION: root") is named before the game starts."""
        update = self.update_dir()
        if not update:
            return True
        try:
            problems = self.update_installer().check_update(update)
        except Exception:  # the check itself must never keep a game from starting
            return True
        if not problems:
            return True
        return self.ask_okcancel(
            "Your game update folder has missing or damaged files:\n\n" + "\n".join(problems[:6]) +
            f"\n\n{update}\n\nThe games will probably stop at start. Extract the update again (or use Install "
            "from .pkg under System -> GAME UPDATE). Play anyway?")

    def confirm_without_update(self):
        """Without update 1.02 most of the port's game fixes (crash fixes among them) are skipped:
        asks once per game folder (a 1.02 copy with the update merged in reports 1.02 itself)."""
        folder = self.game_dir()
        version = app_version(folder)
        if self.update_dir() or not version or version >= "01.02" or \
                self.config.get("warned_no_update") == str(folder):
            return True
        title = folder.parent if folder.name.lower() == "uroot" else folder
        if not self.ask_okcancel(
                f"Your game is version {version.lstrip('0')}, without update 1.02.\n\n"
                "Most of this port's fixes, crash fixes among them, are written for update 1.02, so "
                "without it the games crash more often.\n\nDump update 1.02 from your PS4 and extract it "
                f"next to the game folder as {title.name}-patch (the System section then shows it).\n\n"
                "Play anyway?"):
            return False
        self.config["warned_no_update"] = str(folder)
        write_config(self.config)
        return True

    def confirm_slow_drive(self):
        """The port or the game on a USB, SD or FAT/exFAT drive (slow_drive_reason): asks once per
        pair of folders."""
        places = []
        for label, folder in (("This port", ROOT), ("The game", self.game_dir())):
            try:
                reason = slow_drive_reason(folder)
            except (OSError, ValueError, ctypes.ArgumentError):
                reason = None
            if reason:
                places.append(f"{label} ({folder}): {reason}.")
        key = f"{ROOT}|{self.game_dir()}"
        if not places or self.config.get("warned_slow_drive") == key:
            return True
        if not self.ask_okcancel(
                "\n".join(places) + "\n\nOn such drives the games freeze for up to a second, "
                "again and again, while the port writes its shader cache, logs and saves (Windows writes "
                "to them without a cache).\n\nFor smooth play, copy this port folder and the game folder "
                "to an internal drive (an SSD such as C:) and start the launcher from there.\n\n"
                "Play anyway?"):
            return False
        self.config["warned_slow_drive"] = key
        write_config(self.config)
        return True

    def play_direct(self):
        self.play()
        if not self.process:  # no game folder chosen, or the start failed (already reported)
            self.root.destroy()

    def finished(self, code):
        self.process = None
        self.next_start_noted = False
        self.draw_play()
        reason = self.crash_reason()
        # A freeze is reported however the game was closed (STOP, or Task Manager).
        froze = bool(reason) and reason.startswith("The game froze")
        if (code in (0, None) or getattr(self, "stopping", False)) and not froze:
            self.stopping = False
            self.set_status("Game closed.", "Ready to play again.")
        else:
            self.stopping = False
            self.set_status("The game froze." if froze else "The game stopped unexpectedly.",
                            "A report was saved; see the log folder." if froze
                            else f"Exit code {code}. See the log for details.")
            if reason:
                # 24 Oct 2026 (#148, #163, #172): crashes of a 1.00 game were reported as port bugs;
                # most of the port's fixes are written for update 1.02.
                version = app_version(self.game_dir())
                if not self.update_dir() and version and version < "01.02":
                    reason += (f"\n\nYour game is version {version.lstrip('0')} without update 1.02, so most of "
                               "the port's crash fixes are skipped. Install update 1.02 first (System -> "
                               "GAME UPDATE).")
                self.show_crash(reason)
        if self.direct:
            self.root.after(200, self.root.destroy)

    def show_crash(self, reason):
        """The crash_reason in a themed error: its first line, then the rest in the detail box
        (copyable), with buttons for the log folder and a report to paste into a GitHub issue."""
        first, _, rest = reason.partition("\n")
        choice = Dialog.ask(
            self.root, "The game stopped", first,
            [("Open log folder", "logs", "primary"), ("Copy report", "report"), ("Close", None)],
            detail=rest.strip() or None, cancel=None, scale=self.s, icon="error")
        if choice == "logs":
            self.open_logs()
        elif choice == "report":
            copy_text(self.root, self.crash_report(reason))
            self.toast("Report copied", "ok")

    def crash_report(self, reason):
        game = self.current_game()
        ini = read_ini(game["key"])
        version = updater.installed_version(ROOT)
        log = self.latest_log()
        lines = [reason, "",
                 f"Game: {game['name']} ({game['exe']})",
                 f"Game version: {app_version(self.game_dir()) or '?'}",
                 f"Graphics card: {self.gpu_name or '?'}",
                 f"Resolution: {self.game_settings().get('resolution', '?')}"]
        lines += [f"{key}: {ini.get(key, INI_DEFAULTS.get(key, '?'))}"
                  for key in ("upscaler", "preset", "fps_limit", "display_mode")]
        lines += [f"Launcher: {updater.version_text(version) if version else 'source tree'} ({EDITION})",
                  f"Log: {log or '?'}"]
        return "\n".join(lines)

    def stop(self):
        if not self.process:
            return
        self.stopping = True
        pid = self.process.pid
        # 24 Oct 2026: taskkill of a whole process tree can take seconds; off the Tk thread.
        threading.Thread(target=lambda: subprocess.run(
            ["taskkill", "/PID", str(pid), "/T", "/F"], creationflags=NO_WINDOW,
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL), daemon=True).start()
        self.set_status("Stopping the game...")

    # -- system actions ---------------------------------------------------------------------

    def open_logs(self):
        log = self.latest_log()
        folder = log.parent if log else ROOT / "analysis"
        os.startfile(folder)

    def open_saves(self):
        folder = USER_DIR / "savedata"
        os.startfile(folder if folder.exists() else USER_DIR)

    def clear_cache(self):
        cache = USER_DIR / "cache"
        if not cache.exists():
            self.info("There is no shader cache yet.")
            return
        if self.process:
            self.warn("Close the game first.")
            return
        if not self.ask_yesno("Delete the shader cache?\n\nShaders are compiled again the next "
                              "time you play, which brings back stutters for a while."):
            return
        shutil.rmtree(cache, ignore_errors=True)
        self.toast("Shader cache cleared.", "ok")

    def make_shortcut(self, name, description, play=None):
        """A desktop shortcut to the launcher, or with `play` (u1/u2/u3) to that game directly."""
        link = desktop_folder() / f"{name}.lnk"
        pythonw = Path(sys.executable).with_name("pythonw.exe")
        target = pythonw if pythonw.exists() else Path(sys.executable)
        arguments = f"\"{Path(__file__).resolve()}\""
        if STARTER_EXE.exists():
            target, arguments = STARTER_EXE, ""
        if play:
            arguments = f"{arguments} --play {play}".strip()
        def ps(value):  # a PowerShell single-quoted string ("Drake's" doubles the quote)
            return "'" + str(value).replace("'", "''") + "'"
        # Messages in UTF-8 (a German or Portuguese Windows answers in its own language).
        script = ("[Console]::OutputEncoding=[System.Text.Encoding]::UTF8;"
                  f"$s=(New-Object -ComObject WScript.Shell).CreateShortcut({ps(link)});"
                  f"$s.TargetPath={ps(target)};$s.Arguments={ps(arguments)};"
                  f"$s.WorkingDirectory={ps(ROOT)};$s.IconLocation={ps(ASSETS / 'uncharted.ico')};"
                  f"$s.Description={ps(description)};$s.Save()")
        result = subprocess.run(["powershell", "-NoProfile", "-Command", script], creationflags=NO_WINDOW,
                                capture_output=True, text=True, encoding="utf-8", errors="replace")
        return link, result

    def update_installer(self):
        import importlib.util
        spec = importlib.util.spec_from_file_location("install_update", ROOT / "tools" / "install_update.py")
        installer = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(installer)
        return installer

    def choose_update_folder(self):
        """An update the player already extracted, anywhere: checked and remembered (nothing is
        copied); the games get it as UNCHARTED_UPDATE."""
        if self.process:
            self.warn("Close the game first.")
            return
        game = self.game_dir()
        if not is_game_dir(game):
            self.warn("Choose the game folder first (Change game folder).")
            return
        from tkinter import filedialog
        folder = filedialog.askdirectory(title="Choose the extracted update 1.02 folder", parent=self.root,
                                         mustexist=True)
        if not folder:
            return
        installer = self.update_installer()
        try:
            update = installer.check_folder(folder, game)
        except installer.UpdateError as error:
            self.error("This folder can't be used:", str(error))
            return
        self.config["update_dir"] = str(update)
        write_config(self.config)
        self.update_label.config(text=self.update_text())
        self.refresh_home()
        self.toast(f"Game update {app_version(update) or '?'} is used from now on:\n{update}", "ok")

    def install_game_update(self):
        """The player's own update PKG unpacked next to the game folder (tools/install_update.py;
        the port ships no game files)."""
        if self.process:
            self.warn("Close the game first.")
            return
        game = self.game_dir()
        if not is_game_dir(game):
            self.warn("Choose the game folder first (Change game folder).")
            return
        from tkinter import filedialog
        pkg = filedialog.askopenfilename(title="Choose the game update (.pkg)", parent=self.root,
                                         filetypes=[("PS4 package", "*.pkg"), ("All files", "*.*")])
        if not pkg:
            return
        installer = self.update_installer()
        existing = installer.existing_update(game)
        if existing and not self.ask_yesno(
                f"There is an update folder already:\n{existing}\n\nReplace it with this update?"):
            return
        state = {"step": "Checking the update..."}

        def work():
            try:
                state["path"] = installer.install(pkg, game, log=lambda text: state.__setitem__("step", text))
            except installer.UpdateError as error:
                state["error"] = str(error)
            except Exception as error:  # a disk or permission problem: shown as it is
                state["error"] = f"{type(error).__name__}: {error}"

        worker = threading.Thread(target=work, daemon=True)
        worker.start()

        def poll():
            if worker.is_alive():
                self.update_label.config(text=state["step"])
                self.root.after(200, poll)
                return
            if "path" in state:  # the installed one is used, also over a folder chosen before
                self.config["update_dir"] = str(state["path"])
                write_config(self.config)
            self.update_label.config(text=self.update_text())
            self.refresh_home()
            if "error" in state:
                self.error("The update was not installed:", state["error"])
            else:
                self.toast(f"Update installed:\n{state['path']}\n\nThe games use it from their next start.", "ok")
        poll()

    def install_game_pkg(self):
        """The player's own game PKG (for example the US copy, CUSA02320) unpacked into a folder
        they choose, with the metadata the launcher needs (tools/install_update.py install_game;
        the port ships no game files). That folder becomes the game folder."""
        if self.process:
            self.warn("Close the game first.")
            return
        from tkinter import filedialog
        pkg = filedialog.askopenfilename(title="Choose the game (.pkg), not the update", parent=self.root,
                                         filetypes=[("PS4 package", "*.pkg"), ("All files", "*.*")])
        if not pkg:
            return
        parent = filedialog.askdirectory(title="Choose where to put the game (about 60 GB)", parent=self.root,
                                         initialdir=str(Path(pkg).parent))
        if not parent:
            return
        installer = self.update_installer()
        state = {"step": "Checking the game package..."}

        def work():
            try:
                state["path"] = installer.install_game(pkg, parent,
                                                       log=lambda text: state.__setitem__("step", text))
            except installer.UpdateError as error:
                state["error"] = str(error)
            except Exception as error:  # a disk or permission problem: shown as it is
                state["error"] = f"{type(error).__name__}: {error}"

        worker = threading.Thread(target=work, daemon=True)
        worker.start()

        def poll():
            if worker.is_alive():
                self.game_dir_label.config(text=state["step"])
                self.root.after(200, poll)
                return
            if "path" in state:
                self.config["game_dir"] = str(state["path"])
                if WINE:
                    self.config["game_dir_unix"] = unix_path(str(state["path"]))
                write_config(self.config)
            self.game_dir_label.config(text=f"Game files: {self.game_dir()}")
            self.update_label.config(text=self.update_text())
            self.refresh_home()
            if "error" in state:
                self.error("The game was not installed:", state["error"])
            else:
                self.info(f"Game installed:\n{state['path']}\n\nIt is the game folder now. "
                          "Next, install update 1.02: System -> GAME UPDATE -> Install from .pkg.")
        poll()

    def in_background(self, work, done):
        """Runs `work` off the Tk thread (PowerShell calls take seconds); `done(result)` runs on it."""
        found = []

        def run():
            try:
                found.append((work(), None))
            except Exception as error:  # raised again on the Tk thread, where report_callback_exception logs it
                found.append((None, error))
        threading.Thread(target=run, daemon=True).start()

        def poll():
            if not found:
                self.root.after(200, poll)
                return
            result, error = found[0]
            if error is not None:
                raise error
            done(result)
        self.root.after(200, poll)

    def desktop_shortcut(self):
        def done(made):
            link, result = made
            if result.returncode == 0:
                self.toast(f"Shortcut created:\n{link}", "ok")
            else:
                self.error("Could not create the shortcut:", result.stderr.strip())
        self.in_background(lambda: self.make_shortcut("Uncharted", APP_TITLE), done)

    def game_shortcuts(self):
        """One desktop shortcut per game: starts it directly with the launcher's settings."""
        def work():
            made, failed = [], []
            for game in GAMES:
                name = game["name"].replace(":", " -")
                link, result = self.make_shortcut(name, f"{game['name']} (starts directly)", play=game["key"])
                (made if result.returncode == 0 else failed).append(link.name if result.returncode == 0
                                                                    else result.stderr.strip())
            return made, failed

        def done(outcome):
            made, failed = outcome
            if failed:
                self.error("Could not create the shortcuts:", "\n".join(failed))
            else:  # a Dialog, not a Toast: it explains --play
                self.info("Shortcuts created on the desktop:\n" + "\n".join(made) +
                          "\n\nEach starts its game directly with the launcher's settings. From a command "
                          "prompt: \"Uncharted Launcher.exe\" --play u1 (or u2, u3).")
        self.in_background(work, done)

    def reset(self):
        name = self.current_game()["name"]
        if not self.ask_yesno(f"Reset {name}'s settings to the Medium preset?"):
            return
        self.game_settings().update(GAME_SETTING_DEFAULTS)
        self.game_settings()["pad_controls"] = None
        self.ini.update(INI_DEFAULTS)
        # 24 Oct 2026: the controls go back into the game's ini too; reload_ini (start, game switch)
        # restores them from key_map/mouse_map/pad_map there, so Reset used to leave them as they were.
        self.save_controls()
        self.refresh_controls()
        self.apply_preset("medium")

    def close(self):
        if self.process and not self.ask_yesno(
                "The game is still running. Close the launcher anyway? (The game keeps running.)"):
            return
        self.config["window_xy"] = [self.root.winfo_x(), self.root.winfo_y()]
        write_config(self.config)
        self.root.destroy()


def main():
    try:
        ctypes.windll.shcore.SetProcessDpiAwareness(1)
    except Exception:
        pass
    # Its own taskbar identity: without it the launcher's window is grouped under Python
    # (pythonw.exe) and the taskbar shows Python's icon instead of Uncharted's.
    try:
        ctypes.windll.shell32.SetCurrentProcessExplicitAppUserModelID("espindoIa.Uncharted.Expedition")
    except Exception:
        pass
    # --play u1|u2|u3 (or 1/2/3): start that game directly (desktop game shortcuts, GitHub #25).
    direct = None
    if "--play" in sys.argv[1:]:
        at = sys.argv.index("--play")
        value = sys.argv[at + 1].lower() if at + 1 < len(sys.argv) else ""
        direct = value if value in GAME_KEYS else f"u{value}" if f"u{value}" in GAME_KEYS else None
    root = tk.Tk()
    if direct:
        root.withdraw()
    # Errors in button and timer callbacks go to the error log too (pythonw has no console).
    root.report_callback_exception = lambda kind, value, trace: log_launcher_error(kind, value, trace)
    Launcher(root, direct)
    root.mainloop()


def log_launcher_error(kind, value, trace):
    """Appends an error to analysis/uncharted-user/launcher-error.log: the launcher runs under
    pythonw, so an error at start used to close it without a word (GitHub #21, 0.2.1)."""
    import traceback
    text = "".join(traceback.format_exception(kind, value, trace))
    path = USER_DIR / "launcher-error.log"
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        with open(path, "a", encoding="utf-8") as log:
            log.write(time.strftime("%Y-%m-%d %H:%M:%S ") + APP_TITLE + "\n" + text + "\n")
    except OSError:
        pass
    return text, path


if __name__ == "__main__":
    try:
        main()
    except Exception:
        text, path = log_launcher_error(*sys.exc_info())
        try:
            fallback = tk.Tk()
            fallback.withdraw()
            messagebox.showerror(APP_TITLE, f"The launcher could not start:\n\n{text[-1500:]}\n"
                                            f"This was saved to:\n{path}")
            fallback.destroy()
        except Exception:
            pass
        raise
