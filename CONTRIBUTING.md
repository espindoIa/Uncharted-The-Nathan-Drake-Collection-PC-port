# Commits and pull requests

How changes to the Expedition Edition fork are written down, so that anyone can read the history
and understand what changed, why, and how it was checked, without opening the code.

Three places tell the story, each for its own reader:

| Where | For whom | What it says |
|---|---|---|
| Commit message | Someone reading `git log` | One change: what, why, how it was checked |
| Pull request | Someone reviewing or approving | The whole change set, its risk and its test |
| `CHANGELOG.md` | Players | What they will notice, in plain words |

`analysis/STATUS.md` keeps the engineering detail (measurements, causes ruled out).

## Commit messages

### The subject line

```
<area>: <what is now true, in plain words>
```

- **Area**: one word from the table below, so the log can be scanned by topic.
- **Summary**: what the change does, as the result a reader cares about ("U2 no longer stutters at
  the GPU limit"), not the mechanics ("move PrepareFrame call").
- At most **72 characters**, no full stop, starts with a capital letter after the colon.
- In **English**.

| Area | Use it for |
|---|---|
| `u1`, `u2`, `u3` | A fix or feature for one game |
| `games` | Something all three games get |
| `gpu` | Renderer, upscalers (FSR, DLSS, TAA), draw pipe, shaders |
| `runtime` | The PS4 system libraries, loader, memory, threads, files, saves, pad |
| `launcher` | The launcher's look and options, the updater |
| `package` | `tools/package_windows.sh`, the zip, the starter, CI |
| `tools` | Scripts in `tools/` (probe, play, benchmarks) |
| `docs` | README, CHANGELOG, STATUS, CLAUDE.md, this file |
| `merge` | Bringing in upstream (BilalReloaded) work |
| `release` | Version number, release notes, tag |

Good:

```
u3: no more freezes with multithreaded rendering
gpu: one upscaler frame start per frame in U1 and U3
launcher: Expedition Edition look and compass icon
merge: BilalReloaded's 0.3.1
```

Not good:

```
fixes                                  (what? where?)
Update vk_rasterizer.cpp               (the diff already says the file)
WIP                                    (do not push unfinished work to main)
u3: Fix the bug where the GPU command thread would wait on the flip label before the held
interrupts were delivered which caused...   (too long: the detail goes in the body)
```

### The body

Leave one empty line after the subject, then use these headings. Skip a heading only when it truly
has nothing to say. Wrap lines at 100 characters.

```
u2: no more regular stutter at the GPU limit

Why:
About every fifth frame stayed on screen 30+ ms: a frame waiting for GPU results was prepared
only once they were back, behind the next frame's work.

What changed:
- The frame is prepared at its flip packet (driver.cpp, PrepareFrame).
- A new toggle bit (gow3_toggles.h) brings the old order back for A/B tests.

Tested:
- U2 at 4K DLAA, VSync on, 2 runs each: intervals over 25 ms 18% -> 0.4%.
- U3 at the same settings: unchanged.

Not tested:
- AMD GPUs; Steam Deck.

Co-Authored-By: ...
```

- **Why**: the problem, as the player or developer saw it. Link the issue (`#29`) if there is one.
- **What changed**: the main pieces, one line each. Name files only when it helps find them.
- **Tested**: what was run and what it showed, with numbers where there are any.
- **Not tested**: what could break and was not checked. Saying it is better than hiding it.

### One change per commit

- A commit does one thing that can be explained in its subject. A fix and an unrelated clean-up are
  two commits.
- Every commit builds. Run the checks (below) before committing.
- Never commit game files, dumps, saves, logs, `nvngx_dlss.dll`,
  `gow3_pc/stat-multipliers-test.ini` or the `gow3_pc/gpu/third_party/fsr-vulkan` submodule state.
  One exception (the owner, 25 Oct 2026): the games' UI art (font glyphs, button icons, HUD shapes and
  the textures or archive extracts they come from) may go in `gow3_pc/assets/ui/` only. Executables,
  PKGs, whole pak/psarc archives, saves, dumps, logs and shader caches stay out everywhere.
  Stage files by name (`git add <file>`), not `git add -A`.

`.gitmessage` holds the body layout. To have Git open it for every commit:

```bash
git config commit.template .gitmessage
```

## Branches

- `main` is what players get: it always builds and plays.
- Work that takes more than one commit, or that needs review, goes on a branch named
  `<area>/<short-topic>`, for example `u1/grenade-launcher`, `launcher/controls-page`,
  `merge/upstream-0.3.2`.
- Bring `main` into a long-lived branch with a merge, not a rebase of pushed commits.

## Pull requests

Open one for every branch that goes into `main`. The template (`.github/pull_request_template.md`)
fills in the layout; keep its headings.

- **Title**: the same form as a commit subject (`u1: enemy grenade launchers no longer one-shot`).
- **Summary**: two or three sentences a player could follow.
- **Changes**: the commits in plain words, grouped if there are many.
- **How it was tested**: games, settings and GPU, what you did in the game, and the result.
  Screenshots or frame-time numbers for visual or performance changes.
- **Risk and not tested**: what might break, and what nobody checked.
- **Checklist**: build, tests, CHANGELOG, STATUS, no game files.

Merging:

- **Squash** a branch whose commits are small steps of one change: the squash message then follows
  the commit rules above.
- **Merge commit** for an upstream merge or a branch whose commits each stand on their own.
- Delete the branch after merging.

## Upstream merges

The fork takes work from BilalReloaded's port (`upstream`). A merge commit:

- has the subject `merge: BilalReloaded's <version>` (or the PR's topic),
- lists every conflict and the choice made for it, in the body,
- adds the same list to "Fork (espindoIa) notes" in `analysis/STATUS.md`.

Before merging, look for patch tables both sides added in `tools/probe_uncharted.py`: a patch
applied first keeps a later one's signature from matching.

## Releases

1. Set `tools/VERSION` and write `tools/release-notes/v<version>.md` (player-facing; the v0.4.0
   notes show the layout).
2. Give the `CHANGELOG.md` section its version number.
3. Commit as `release: <version>`, tag `v<version>`, push both.
4. Build the package (CLAUDE.md, "Package") and publish it:
   `gh release create v<version> dist/Uncharted-windows.zip --notes-file tools/release-notes/v<version>.md`.

## Checks before a commit or pull request

- Runtime builds: `gow3_pc/build.sh` (MSYS2 CLANG64, see CLAUDE.md).
- The unit tests touched by the change (`ninja -C out/gpu <test> && ./out/gpu/<test>.exe`).
- Launcher: `python -c "import ast; ast.parse(open('launcher/uncharted_launcher.pyw', encoding='utf-8').read())"`.
- A short start of the game(s) the change affects, when it changes behaviour in game.
