# Roadmap to 1.0

The road from 0.4.0 (19 October 2026, the first Expedition Edition release) to 1.0. Each version
has one theme, a list of work and a **done when** line: the version ships when that line is
true, not on a date. What has shipped is in [CHANGELOG.md](CHANGELOG.md), the engineering detail
in [analysis/STATUS.md](analysis/STATUS.md). Issue numbers (#n) are on
[BilalReloaded's repository](https://github.com/bc160201983/Uncharted-The-Nathan-Drake-Collection-PC-port/issues),
where players report; this fork has no issue tracker yet.

## What 1.0 means

All of these must be true:

1. **All three games can be finished**, start to credits, on the US (CUSA02320) and Europe
   (CUSA02344) versions with update 1.02, using the launcher's defaults.
2. **No known crash or freeze that has a way to reproduce it.** Bugs that happen once and cannot
   be repeated are listed as known issues with their logs.
3. **Multithreaded rendering is on by default in all three games** and has passed the soak runs
   (below) in each.
4. **Performance targets are met** (measured with `tools/play.py` and `C:\dev\bench`, A/B in one
   session):
   - 60 FPS at 1080p in the heaviest bench scene of each game on a mid-range PC (6 cores, an
     RTX 3060 class GPU).
   - 4K output at the 120 FPS limit in Drake's Fortune and Uncharted 2, and at least 90 FPS in
     Uncharted 3, on an RTX 5070 with DLSS Quality.
5. **The picture matches the PS4** in the scenes checked so far: no flicker, missing effects or
   validation errors left that change what the player sees.
6. **A new player can install and play from the release page alone**: the fork builds its own
   package on GitHub Actions, the launcher updates from the fork's releases, and the README
   covers setup, DLSS and the update folder.

## 0.5: upstream 0.4.0 and the reported crashes

Theme: catch up with upstream, then fix what players hit most.

- **Merge upstream 0.4.0.** It adds:
  - real upscaling, with the HUD drawn at full size (kuraeow, upstream PR 50);
  - the fix for freezes before some cutscenes (#52: 1.6 s on PC, 33 s on Steam Deck);
  - Drake's Fortune's grenade launcher fix (#6).
  
  Overlaps to choose between:
  - the fork's DLSS/FSR quality modes against upstream's upscaling;
  - the fork's multithreaded freeze fixes against upstream's fix for #51 (upstream also turns
    multithreading off for Uncharted 3);
  - duplicate patch tables in `tools/probe_uncharted.py`.
  
  Version name (decided 19 Oct): the next fork release is **0.5.0 "Expedition"**. Upstream also
  has a 0.4.0; the launcher checks only the fork's releases, and the "Expedition" name tells
  players which one they have. Done: merged in 4ecc8fd.
- **Crash triage from the open upstream issues**, each reproduced or ruled out, with a STATUS note:
  - "Exit code 3" / "ErrorCode3" in all games (#24, #27, #30, #59);
  - Drake's Fortune crash at start or in the first levels (#42, #58);
  - Uncharted 2: chapter 11 (#41), before the guards chase Drake (#33), stutter and crash (#56);
  - Uncharted 3 (#44, #46, #47, #54).
- **Drake's Fortune "Device lost" after 26 minutes** (#57). The log shows a 960x540 image with
  6,755 layers (15.8 GB) being created. Reject impossible image descriptions in the texture cache
  instead of allocating them. Done 19 Oct (d3b1e13): the size wrapped in u32 past the 1 GB check.
- **Open issues on the fork** with a bug report template asking for `boot.log`, the launcher
  settings, the game version and the GPU. Done 19 Oct (`.github/ISSUE_TEMPLATE/bug_report.yml`).

Done when: the merge is in; each crash issue above is fixed, has its cause written in STATUS, or
needs more information from its reporter; and a 30-minute run of each game at the player's
settings (4K, DLAA, 120 FPS, multithreading on) ends without a crash.

## 0.6: a correct picture

Theme: what the player sees is right.

- **Vulkan validation:** fix the 7 errors seen in Uncharted 3, among them two sync hazards on the
  1080p depth target (STATUS, 19 Oct).
- **Flickering bottom edge with FSR or TAA** on AMD GPUs (#49).
- **Drake's Fortune:**
  - the smoke blob and the black circles on bullet hits (needs a frame capture);
  - grey guns with multithreading (#23): confirm the fork's fix in a gunfight and at the boats.
- **Uncharted 3:** Sully's shirt in the bar flickers at 60 FPS.
- **Cutscene movies at 16:10 and ultrawide:** check the black bars and the scaling. Also fix the
  `videodec2-test` build (it is missing `core/memory.h`) so the movie path has a test again.
- **Optional intro logos** (#55): a launcher switch to keep the studio logo movies at start.

Done when: validation in each game shows no error tied to something visible, and each item above
is fixed or recorded as a known issue with the reason.

## 0.7: performance

Theme: the frame-rate targets in "What 1.0 means".

- **Draw recording thread (stage B).** With DLSS model K or Quality, Uncharted 3 is held there at
  about 61 FPS. Make it faster, then measure again the work that was set aside while it was the
  limit: merging `pipe_write_ranges` (it overflows at 256 dispatches; Uncharted 3 has about
  1,230 a frame).
- **Guest writes into GPU-written pages:** about 140 a second, each waiting about 1.4 ms on the
  GPU command thread.
- **Shader stutter:** compile everything the pipeline cache knows before the first frame, so the
  first minutes play smoothly too.
- **4-6 core CPUs:** thread priorities, so the GPU command thread keeps a core next to the game's
  six job threads.
- **Drake's Fortune at native 4K** (it renders at most 2880x1620 now because of its memory
  layout). Only if a safe memory placement is found; otherwise it stays a known limit.
- **Launcher defaults per GPU class:** at 4K, DLAA with DLSS model M costs 7.8 ms a frame and
  model K 2.2 ms. Pick the preset defaults from those measurements.

Done when: the performance targets in "What 1.0 means" are met, each with an A/B entry in STATUS.

## 0.8: full playthroughs

Theme: prove every game can be finished.

- **Play each game to the credits** on 1.02, on CUSA02320 and on CUSA02344, with the defaults.
  Record a `tools/play.py` route or save at each chapter where something went wrong.
- **Regression suite:** a fixed set of saves and replay routes per game, for example the opening,
  a gunfight, a cutscene and the heaviest bench scene, run with `tools/play.py` before every
  release. It compares screenshots and frame times with the last release.
- **Soak runs:** 2 hours of play per game with multithreading on, at both the player's settings
  and the 1080p Medium preset.
- **Saves and switching between games:** saves made on 1.00 load on 1.02; switching games from
  the Collection menu works throughout a playthrough.

Done when: all six playthroughs (3 games x 2 regions) are finished, the soak runs end without a
crash, and the regression suite runs in one command.

## 0.9: release candidate

Theme: ready for players who never read the code.

- **The fork builds its own package:** GitHub Actions on the fork produces
  `Uncharted-windows.zip` and the release, as upstream's does. There have been no runs on the fork
  so far.
- **The launcher updates from the fork's releases**, end to end: an older fork package offers the
  update, downloads it, checks the SHA-256 and keeps the saves.
- **First run:** clear messages for a missing game folder, a wrong dump (1.00 or 1.02, region),
  a missing `nvngx_dlss.dll` with DLSS chosen, and a GPU without Vulkan 1.3.
- **Documentation:** the README's status table, requirements and known issues are rewritten from
  the 0.8 results. CHANGELOG entries since 0.4.0 are reviewed for player-facing wording.
- **Freeze for features:** from 0.9 on, only fixes go in.

Done when: a clean Windows account can install the package and play each game from the README
alone, and two weeks pass on 0.9.x with no new crash that has a way to reproduce it.

## 1.0

Ships when every point in "What 1.0 means" is true. The release notes list the known issues
that are left, each with its log or the reason it was not fixed.

## Not planned for 1.0

- Replacing the shadPS4 renderer with another GPU layer (#53). This is a research question,
  worth looking at after 1.0.
- Frame rates above 120 FPS: the games' movement breaks above that.
- Renaming the internal `gow3` names. This is cosmetic and touches every file.
- Linux and Steam Deck as supported targets. Upstream's Proton fixes are merged and kept working,
  but there is no testing on them here.
