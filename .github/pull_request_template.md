<!-- Title: <area>: <what is now true>  e.g. "u1: enemy grenade launchers no longer one-shot".
     Areas: u1 u2 u3 games gpu runtime launcher package tools docs merge release (CONTRIBUTING.md). -->

## Summary

<!-- Two or three sentences a player could follow: what changes for them, and why. Link issues (#29). -->

## Changes

<!-- The commits in plain words, one line each, grouped if there are many. -->

-

## How it was tested

<!-- Game(s), settings (resolution, upscaler, multithreading), GPU, what you did in the game, and the
     result. Screenshots or frame-time numbers for visual or performance changes. -->

-

## Risk and not tested

<!-- What might break, and what nobody checked (other GPUs, Steam Deck, the other games...). -->

-

## Checklist

- [ ] Runtime builds (`gow3_pc/build.sh`) and the touched unit tests pass
- [ ] Launcher syntax check passes (if the launcher changed)
- [ ] `CHANGELOG.md` updated in player words (if behaviour changed)
- [ ] `analysis/STATUS.md` has the technical detail (if behaviour changed)
- [ ] No game files, saves, logs or `nvngx_dlss.dll` in the diff (the games' UI art only under `gow3_pc/assets/ui/`)
- [ ] Commit messages follow CONTRIBUTING.md
