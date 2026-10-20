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
