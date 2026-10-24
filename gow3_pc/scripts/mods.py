#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Links for game_update.py's merged game folder, preserving the original files."""
import os
from pathlib import Path
import shutil

# Windows without the symlink privilege (Developer Mode off): directories become junctions,
# files hard links (copies across volumes). Those are not symlinks, so the overlay remembers
# what it linked and where to.
LINKED = {}


def make_link(link, target):
    # gow3 24 Oct 2026 (#43 ...): files as hard links first (same drive, no privilege needed, and
    # every stat sees the real size; a file symlink stats as 0 bytes in Windows 11's UCRT).
    if os.name == 'nt' and target.is_file():
        try:
            os.link(target, link)
            LINKED[str(link)] = target
            return
        except OSError:
            pass  # another drive: a symbolic link (runtime_file.c follows it) or a copy
    try:
        link.symlink_to(target, target_is_directory=target.is_dir())
        return
    except OSError:
        if os.name != 'nt':
            raise
    if target.is_dir():
        import _winapi
        _winapi.CreateJunction(str(target), str(link))
    else:
        try:
            os.link(target, link)
        except OSError:
            shutil.copy2(target, link)
    LINKED[str(link)] = target


def is_link(path):
    return path.is_symlink() or str(path) in LINKED or (hasattr(path, 'is_junction') and path.is_junction())
