# Changelog

Uncharted: The Nathan Drake Collection PC port, by **BilalReloaded**
([YouTube](https://www.youtube.com/@BilalReloaded/videos), [GitHub](https://github.com/bc160201983)).
Technical details of every item: [analysis/STATUS.md](analysis/STATUS.md).

## Unreleased (fork, after 0.5.0)

- **DLAA: sharper textures (hair, walls).** DLSS at native resolution (DLAA) now uses NVIDIA's
  recommended texture detail bias (-1, the same rule Performance and Quality already followed).
  In the Uncharted 3 bar scene a wall's fine detail went from 5.3 to 13.4 on our sharpness measure,
  with no change in DLSS cost (2.27 ms against 2.28 ms). NVIDIA warns that this can add shimmer or
  moire in motion. Not tested: motion, Uncharted 1 and 2, heavy scenes. Toggle bit 4096 in
  `disabled-optimizations.txt` (second number) restores the old look without a restart.

- **Merged: BilalReloaded's 0.5.0 (21 October), with its new features.** Now in the fork: 5.1 and
  7.1 speakers (launcher Advanced -> Speakers), Unlock all rewards, trophies with a popup and a
  menu tab, the controller light bar, mouse aim options (sensitivity, up/down speed, invert
  left/right) with upstream's smoother aiming, Install game from .pkg, the warning for USB, SD and
  exFAT drives, video memory given back when unused, pageable memory on cards with little video
  memory, the fix for the command-list memory leak, and the shader cache checksum and repair (and a
  cache kept per graphics card). The details are in the section below. Where the fork already had
  its own fix (camera and object motion, multithreaded rendering on in all three games, the
  launcher's saved settings) the fork's version stays. Tested: the build; 30-45 s boots of
  Uncharted 1, 2 and 3 with frame stats; the Uncharted 3 benchmark in the chapter 2 meat market:
  76.6 FPS (about 75 before the merge, so no regression). Also fixed here: a freeze at the start of
  gameplay in Uncharted 3 that the merge itself caused (see STATUS.md). Not tested: the new
  features themselves in a game, a long session, mouse look and aim after upstream's mouse
  rewrite, the multithreaded flicker (#23).

- **Fixed: Drake's Fortune "out of device memory" after about an hour, and the Uncharted 2 crash
  reading a border colour table at address 0.** Ported from BilalReloaded's 0.4.1 (upstream #63 and
  #56). Images with more layers than the GPU allows or over 1 GB are now skipped, and a border colour
  table at 0 or unreadable gives transparent black. Tested: the build and 40 s boots of Uncharted 1
  and 3 with no exception. Not tested: the hour-long Drake's Fortune session, Uncharted 2's crash
  scene.

- **Fixed: the common Drake's Fortune crash "execute access to 0".** Ported from BilalReloaded's 0.5.0
  (upstream #27, #30, #42, #62; Uncharted 2 and 3 have the same code). The pages of the shader
  constant table are now written once before the table is filled. `UNCHARTED_CUE_PRETOUCH=0` turns
  it off. Tested: 40 s boots of Uncharted 1 and 3 (update 1.02), patch applied, no exception. Not
  tested: Uncharted 2, and the crash itself (it needs the reporters' Windows 10 stack layout).

- **Improved: idle job workers now also sleep on the other retail 1.02 build (EP9000-CUSA02344).**
  Ported from BilalReloaded's upstream commit 64abc3f (Supermedo/uncharted_pc #346). Tested: boots of
  Uncharted 1 and 3 on our dump patch the same offsets as before. Not tested: the EP9000 build
  itself.

- **Fixed: the screen wipe in Uncharted 2 and 3 left a band at the bottom below 720p (540p on Steam
  Deck).** Ported from BilalReloaded's upstream commit 696bcb1. The wipe now uses the PS4's
  1920x1080 when the resolution patch is on. Tested: the patch script parses. Not tested: a
  sub-720p run (the boots did not apply the resolution patch).

### From BilalReloaded's 0.4.1 and 0.5.0 (merged 21 Oct)

Upstream's 0.5.0 notes (21 October 2026), in their words. Not repeated here: both 0.4.1 fixes
and, from 0.5.0, the "execute access to 0" fix, the job-sleep fix for another 1.02 build (#87)
and the screen wipe below 720p: they are ported above.

- **Wrong-looking surfaces from a damaged shader cache are repaired**: in Uncharted 3's museum the
  tiled floor of one room showed as a flat red surface on one PC and correctly on another. The
  shader cache keeps each shader version as two files, a description and the compiled code. A
  session had rewritten the code file while Windows refused to replace its description (another
  thread was reading it), so every later start drew that floor with another version's code.
  Descriptions now carry a checksum of their code; a pair that doesn't match (or, in caches from
  earlier versions, whose files were not written together) is thrown away and compiled again
  once. On the affected PC exactly one pair out of about 20,000 was damaged, and the floor now
  shows its pattern.
- **Stutter when the game streams in the next area is gone on most laptops and iGPUs**: when a
  level part loads into memory the GPU had used, the game rewrites it, and on graphics cards whose
  memory the CPU can read directly (Resizable BAR, most current laptops; integrated GPUs) the port
  fetched the GPU's bytes one 4 KiB page per write fault: thousands of faults in a few seconds,
  50-180 ms frames (a laptop in Uncharted 3's Cartagena: 31-46 frames over 40 ms in two
  minutes). Each fault now fetches the finished GPU pages of its 256 KiB window in one copy, and
  the rest of the window is written without faults: 0-4 such frames in the same spot, none from
  this. Drake's Fortune had the same stalls.
- **Multithreaded rendering no longer makes objects flicker**: with it on, whole objects drew
  flat white for single frames in Uncharted 2 (a train car, Drake's gun, enemies; 6-13 times in
  10 seconds of combat on a busy CPU) and weapons flat grey in Drake's Fortune (#23). With it on,
  the port does a small constant copy of the game's on a second thread, and the next draws could
  read its destination before it was written. Those draws now wait for that copy (a few dozen
  times a second at most). Measured: 0 flickering frames in all three games, no FPS cost.
  (combined with the fork's earlier ring-write fix; the fork also keeps multithreaded rendering on
  in Drake's Fortune, which upstream forces off, so the launcher setting decides there.)
- **Drake's Deception: multithreaded rendering on by default**: the port had kept it off in
  Uncharted 3 since the freeze of #51 (fixed in 0.4.0). In a heavy Cartagena spot on a
  laptop, where one CPU thread prepares every draw, it is the difference between 53.7 FPS with 11
  stalls over 100 ms (off) and 60 FPS with 1 (on); no freeze in 15-20 minute tests. The launcher
  turns it on once for Uncharted 3 (Advanced -> Multithreaded rendering turns it off again).
  (the fork keeps its own fix: multithreaded rendering is on by default in all three games since
  fork 0.4.0, and the launcher does not turn it on again for Uncharted 3, so a saved "off" stays off.)
- **Shader cache kept per graphics card**: a laptop that runs the game on its integrated GPU one
  time and on its graphics card the next lost the whole shader cache at every switch (4,478 files
  on a laptop, then half-second compile freezes for minutes). The other card's cache now
  waits beside the current one and comes back when you switch back (at most two are kept; if an
  older version ran in between and left two caches of one card, the newer one comes back).
  (the other card's cache is kept per game, in the fork's per-game cache folders.)
- **Keyboard prompt for Uncharted 3's d-pad picture**: the d-pad prompt with its up arm lit
  (young Drake in the museum, and wherever the game shows it) still showed the PlayStation
  d-pad with the keyboard and mouse. It now shows the key bound to d-pad up on the lit arm.
- **Aiming blur above 1080p is gone again (Uncharted 2 and 3)**: when you aim, the game measures
  the distance under the crosshair to set its depth of field. Above 1080p the port moves that
  measuring spot to the crosshair, but the corrected position sometimes didn't reach the GPU, so
  the focus kept jumping between your target and the wall beside Nate (blur while aiming). The
  correction now always reaches the GPU (checked at 4K: every frame corrected).
- **Warning for USB, SD and exFAT drives**: with the port and the game on an external USB drive
  (an NVMe SSD in a USB case, exFAT), a laptop froze for 0.2-0.8 s dozens of times a
  minute, because Windows writes to such drives without a cache and the port writes its shader
  cache, logs and saves while you play. On the laptop's internal drive the same test had no
  freeze over 100 ms. The launcher now says so before you play from such a drive (once per
  folder), so you can copy the port and the game to an internal SSD.
- **Fewer multi-second freezes in all three games** (#71, #79): an effect and crowd-skinning
  compute shader the three games share reads some of its buffer slots from floats when an effect
  doesn't use them. The port already bound those as empty buffers, but it still built a separate
  version of the shader for every new set of float values, compiled in the middle of the frame:
  one shader had collected 1,177 versions, and single frames took up to 7.8 s here and 10 s on a
  laptop. Those slots now count as empty when the shader version is chosen, so nothing new is
  built for them. The same bug made memory rise through a session (#95): a 0.4.1 log shows
  Uncharted 2 holding 9,942 compute pipelines after a few minutes, where the port now holds
  about 30. A shader that still builds 100 or more versions is now named in the log.
  (combined with the fork's own filter for such buffers, which also catches the unmapped ones
  and stays switchable in the toggle file; that switch now turns both off.)
- **Faster start, and no more crash while loading shaders, for players of earlier versions**
  (#92, #95): the shader cache had kept every version those freezes built, and the loading at
  start rebuilt them all each time: 23,845 compute shaders in one player's cache (the AMD driver
  crashed near the end of loading them), a 332-second load in another. None of those versions is
  used again. At the first start of this version, a shader with 100 or more cached versions is
  left out of the loading and its files are removed (the log names it); the one or two versions
  the game really needs are built again when first used. On our PC that removed 1,651 of 1,730
  compute shaders; all three games then run with 82, and the next start finds nothing to remove.
- **Uncharted 2: crash on a garbage texture fixed** ("image_info.cpp UpdateSize: Assertion
  Failed"): the game sometimes points a draw at data that isn't a texture. One such description,
  a compressed format with a tiling layout compressed textures never use, made the port stop
  while working out its size, before the check that skips garbage textures. It now has no size
  and is skipped like the others. Found in a 16-minute session on a laptop. A test
  that turned 3,000 textures into that one reproduced the crash on the previous build; this
  version kept running at 60 FPS. The same test found a second such stop (a garbage texture read
  as a depth texture), fixed the same way.
- **Video memory no longer only grows** (#95): the port kept every buffer range the game ever
  used in video memory. On a laptop, 16 minutes of Uncharted 2 with one level change
  took that from 320 MiB to 1.9 GB, never less, and through a long session cards with 4-8 GB
  fill up (then the driver moves memory to system RAM and the frame rate falls). Now, above a
  quarter of the card's memory, ranges the game has not used for a minute are given back and
  that memory is used again first. Ranges holding data only the GPU has are never given back.
- **Less "out of device memory" on cards with little video memory** (#81): once the card's own
  memory was full, the driver refused more, and the port stopped. On drivers that support it
  (NVIDIA's do), the driver may now move memory to system RAM instead: slower while the card is
  full, but the game goes on. (Checked with the driver; not yet on a small card itself.)
- **5.1 and 7.1 surround sound**: the three games have their own stereo, 5.1 and 7.1 mixes,
  and pick one from the speaker setup the system reports. The port always reported stereo, so
  only the front left and right speakers ever had sound. It now reports your Windows output
  device's setup (launcher Advanced -> Speakers: Auto, or Stereo / 5.1 / 7.1): on a 5.1 or 7.1
  system the centre, subwoofer and surround speakers play the games' surround mix. Stereo
  outputs and headphones keep the games' own stereo mix (a 5.1/7.1 mix chosen by hand is folded
  to stereo there).
- **Unlock all rewards** (launcher Advanced, off by default): every reward in Bonuses (skins,
  weapons, render modes, art galleries, gameplay tweaks such as Slow Motion, Mirror World or One-
  Shot Kills) and every story cutscene, in all three games, and rewards that are normally enabled
  only after finishing the game can be enabled right away. Uncharted 2 and 3 use Naughty Dog's own
  "give all" switches; Drake's Fortune's reward checks are patched. Nothing is written to the save:
  turned off, the menus show your own progress again. Treasures, medals and trophies are not
  touched. Needs update 1.02.
- **Trophies**: the games award trophies as on the PS4, but the port only wrote them to the log
  and told the games at every start that none had been earned. Earned trophies are now saved
  (in the user folder, trophy/), the games are told which you have, and a new one shows a
  PS5-style popup in the top right corner (below the FPS counter when it is there) with the
  trophy's picture from your copy of the game and a short chime. The in-game menu (Insert) has a
  Trophies tab with every trophy of the running game, earned ones lit. The trophy names are
  encrypted in the game files and are not shown. Trophies earned with earlier versions were not
  saved, so the count starts at zero.
- **Controller light bar**: the games set the DualShock 4 / DualSense light bar (blue at the
  start, then their in-game colours; Uncharted 2 also turns it red). The port ignored that, so
  the light bar stayed as the driver left it. It now shows the games' colours.
- **Smoother mouse aiming in all three games**: while aiming, Uncharted 2 and 3 turn their aim
  camera by the right stick, but the mouse was mostly sent to the normal camera then, so aiming
  with the mouse was choppy. While aiming the mouse is now the stick, and its speed is converted
  with the inverse of the games' stick response, so the turn follows the mouse evenly (slow
  movements turned too little before, fast ones too much). New mouse options (in-game menu
  Controls tab and the launcher): aim sensitivity, up/down speed and invert left/right.
- **Memory no longer grows the whole time you play**: each nested GPU command list kept a small
  piece of memory forever, about 140 MB a minute in Uncharted 3 (over 8 GB in an hour of play,
  enough to run a 16 GB PC out of memory). In the same test, memory now stays flat.

- **US version (CUSA02320) supported**: tested from the US PKGs (base and update 1.02): Uncharted 1,
  2 and 3 start and play (New Game in each, 60 FPS). Its 1.02 executables are byte-identical to the
  European ones (CUSA02344), so every fix and patch of the port applies. Install it with the new
  Install game from .pkg below. If your US copy stopped at the start before (#29, #43, #58, #73),
  please try this version and send launcher-last-run.log if it still does.
- **Install the game from its .pkg** (launcher Advanced -> FOLDERS -> Install game from .pkg): for
  players who have the game as a PKG file (for example the US version, CUSA02320) instead of an
  extracted folder. It unpacks it into a folder you choose, with the files the launcher and the
  update installer need (unpacked by hand, those were missing and the update could not be
  installed), and makes it the game folder. Then install update 1.02 with GAME UPDATE -> Install
  from .pkg. The US version's game files are the same as the European ones, so every fix applies.

From kuraeow (Steam Deck), merged:

- **Motion vectors for animated characters** (#84): the temporal upscalers (DLSS, FSR 3.1/4) got
  the camera's motion only, so moving characters' faces, hair and clothes smeared below Quality.
  Characters now carry their own motion (measured in Uncharted 3 at 960x540 -> 1080p: half the
  error with the camera still, a fifth in a fast turn). GOW3_CHARACTER_MOTION=0 turns it off.
  (the fork keeps its own fix: its flat camera motion with object motion stays the default;
  character motion is used only with UNCHARTED_MOTION_SOURCE=velocity.)

## 21 October 2026 (fork 0.5.0, Expedition Edition)

- **Not fixed yet: the frame rate drop in heavy Uncharted 3 areas (chapter 2, after the movie).** In
  the player's log the game ran at 52-80 FPS with 1,600-3,000 draws and 1,600-1,900 compute
  dispatches per frame. The limit is the CPU: the thread that records the graphics work was 83% busy
  and the graphics command thread, which feeds it, waited 20% for it and was idle only 0.2%. This is
  the CPU limit already listed in the 20 October notes. A profile in the bench puts the time in the
  per-dispatch work: binding buffers, bookkeeping of memory ranges, copying constants. One try
  (skipping repeated bookkeeping of a memory range already recorded in the same step) was used 0.1%
  of the time and gave no gain (75.68 FPS with it, 76.79 without, 3 + 3 windows of 10 s), so it was
  dropped. A real gain needs bigger changes (batching that bookkeeping per submission, reusing
  constant data across submissions, splitting the recording thread); none done. The stall detail
  line also caught a 266.1 ms stall in the graphics driver in the bench: video memory was at 31%
  (3423 of 11175 MB) and no shader compile was running, so it is neither of those. Still open; it
  needs a Windows GPU trace.

- **Improved: the stutter when an area loads in Uncharted 3 (the alley after the chase checkpoint).**
  The player's log had frames of 100.7 and 80.3 ms, with 89.4 and 74.0 ms of them in write traps.
  When the game read a file over memory the graphics card had also written, the port first fetched
  the old graphics-card bytes of every 4 KB page, one queued job per page, although the read
  replaces the whole page (that stretch: 239 file reads, 92.1 MB, 5,323 pages, 468.9 ms of
  preparation). Now whole pages are dropped in one job per read. Pages where that is not safe (a
  pending readback, an image the graphics card changed, the edge pages of a read) still use the old
  way. In the bench (chapter 2 meat market save, Continue) the preparation of the file reads took
  2.2 ms (57 reads, 20.4 MB, 427 pages, 339 dropped) instead of 50.1 ms (67 reads, 23.7 MB, 532
  pages). Known limit: if a read fails or ends short after the pages were dropped, the part it did
  not write loses its graphics-card bytes. One bench run ended during the chapter load without a
  crash message (cause unknown; the next two runs passed). Not tested on the player's own alley
  load, nor in Uncharted 2 or 1, nor with the new code switched off for comparison.

- **Improved: the stall right after a pre-rendered movie in Uncharted 3.** In the player's log, the
  first frame after a movie of about 60 s took 99.7 ms (63.1 ms of it refreshing 17 images, 21.3 MB)
  and five more frames took 42-55 ms. During the movie the port freed all its large upload buffers
  (14, 69 MB; it frees them after 3000 idle frames, about 25 s) and had to create 20 again. Creating
  one is slow: in the bench 38.9 and 33.2 ms for 64 MiB, 30.2 ms for 32 MiB, 15.4 ms for 24 MiB, and
  9-13 ms for a 16 MiB block. The chapter start, with buffers still kept, uploaded 105 MB with 4.7
  ms of refresh. Now the most recently used idle large buffers are kept, up to 128 MiB per memory
  type and 32 buffers (`GOW3_STAGING_KEEP_LARGE_MB`, 0 gives the old behaviour); that can use up to
  128 MiB more memory per type. Not verified at the player's movie (no save before it). Also new in
  the stall log line: the refresh time split into staging allocations, image copies and detiling;
  "Slow staging allocation" lines (5 ms or more); and the Windows "us/draw" in the frame stats is now
  real (it printed 0.00).

- **Fixed: the hitch at the very start of gameplay with DLSS (not DLAA) in Uncharted 3.** DLSS built
  its internal setup (NVIDIA's "feature") when the first gameplay frame was drawn. With a scaling
  preset that took 80-177 ms in our tests, so that frame lasted 90-190 ms, and in the player's log
  230 ms (Performance preset, 4K). DLAA builds faster (about 40 ms). It happens once per session.
  The port now builds it while you are still in the boot and menus, using the size and quality the
  first gameplay frame will need; if the settings turn out different, it builds again then, as
  before. In a test with the player's save and settings (4K) the setup took 63 ms during the menus
  and the chapter start had no stall frame (95.6 FPS, no crash). Not tested in Uncharted 2 or 1, with
  a custom render resolution (the likeliest case to still hitch), or after changing the upscaler in
  the in-game menu.

- **Diagnostic: a rare long stall inside the graphics driver is now logged in detail.** The player's
  log had one 188 ms stall while sending work to the graphics card during a chapter load. It did not
  happen in 3 test runs at the player's settings, so it is not fixed. When it happens again, the log
  gets a "Slow submit detail:" line (for waits of 50 ms or more) with video memory use against its
  budget, shader compiles running at that moment and the compile time of the last 5 seconds. That
  tells whether the stall comes from video memory running out or from shader compiles competing with
  the driver, so the next fix can aim at the right one. Nothing changes in play. Please send the log
  if you see a stall like it.

- **Improved: smoother play when the game writes next to data the graphics card just wrote.** The
  game does this about 1,700 times a second in Uncharted 3, and the port made the game thread wait
  for the graphics card on many of those writes even when they touched different bytes. It now waits
  only when the write overlaps data the graphics card wrote. In the Uncharted 3 bench, standing in
  gameplay, the port's handling of these writes took about 1.8-1.9 s of game-thread time per 5 s,
  now about 0.65 s. The frame rate in that spot did not change (70 FPS, the graphics card is the
  limit there); the gain is expected in CPU-heavy scenes and loads. Not tested at the player's own
  spots, nor in Uncharted 2 or 1. Also new: `GOW3_FAULT_STATS=1` prints more diagnostic lines about
  these writes, and the log now has a line with the time DLSS took to create its feature.

- **Improved: loading a new chapter in Uncharted 3 stutters less.** While a chapter loaded, the game
  reading a file into memory the graphics side was watching made the port stop on one trap per 4 KB
  page, thousands per second. A log from the player's install showed frames of 103 ms and 215 ms spent
  almost entirely there, and one 5 second stretch at 39.9 FPS. The port now prepares the whole
  destination of a file read once instead. This removes the trap itself, but part of the cost is the
  work done for each page, so it only reduces this stutter, it does not remove it. Not fixed: stalls
  of about 97 ms from uploading large images and a 188 ms driver stall seen in the same log (now
  logged in detail). The 230 ms frame in that log was the DLSS start-up, see the entry above. Not
  tested on the player's own chapter load, nor in Uncharted 2 or 1.

- **Fixed: freezes of 1-6 seconds after some cutscenes in Uncharted 3 and 2.** After a cutscene the
  treasure effect (two compute shaders) read data from table slots that hold plain numbers instead of
  buffer descriptions. The port already ignored those slots when drawing, but its shader cache still
  treated every random value as a new variant, so almost every frame compiled a new one on the
  graphics thread (about 20 ms each). Logs from the player's install showed a 6.1 s freeze (263
  compute pipelines), then 4.0 s, 2.2 s and 1.8 s ones in Uncharted 3, and 2.2 s and 1.0 s ones in
  Uncharted 2, every session. Such slots now count as empty for the shader cache too, so one variant
  is enough. Variants already in your cache still match, so nothing is recompiled and the cache does
  not need to be cleared; the old extra variants stay in the file and load in the background without
  a freeze. Not tested at the player's own cutscene spots, and not in Uncharted 2.

- **Fixed: Uncharted 3 chapter 2 no longer gets stuck at a lower frame rate after the alley
  checkpoint.** A buffer the port uses for small per-draw data could fill up and stay full for the
  rest of the session. The drawing thread then copied every small piece of data again, and a log of a
  session in the chase section stayed at 41-49 FPS for over 4 minutes (55-63 FPS before the
  checkpoint). In a test walk to the meat market exit the old build got stuck in 3 of 5 sessions and
  the fixed one in 0 of 3 (few sessions). There the difference was 3-5 FPS, about run-to-run noise,
  because that view is light. In the heavier chase views the gain is estimated at about 6-9 FPS, not
  measured. The chapter is still limited by the CPU in busy streets (62-73 FPS at 4K with DLSS
  Performance); that part is not fixed.
- **Each game keeps its own shader cache.** Uncharted 1, 2 and 3 share one title ID, so all three
  used one cache folder (U3's warm-up read 2351 entries, kept 576 and took 30.85 s). Each game now
  uses `analysis\uncharted-user\cache\CUSA02320-<executable>`. **Each game copies the old shared
  folder into its own once, the first time it opens, so it does not rebuild its shaders.** The old
  shared folder `analysis\uncharted-user\cache\CUSA02320` is left in place; delete it if you want
  the space, after each game has been opened once.
- **Shaders compiled in earlier versions are kept after the update.** The first time a game opens
  its own shader cache, the old shared one is copied into it once (only if the game has no cache
  folder yet). This removes the first-play freezes of up to 5 seconds that the empty cache caused,
  for example right after the museum cutscene in Uncharted 3 chapter 2.
- **Small CPU and GPU savings.** Game clock reads and mutexes no longer take shared locks, a
  memory-protect call that changes nothing no longer stalls the other threads, and each frame skips
  clearing the whole 4K image when the picture already covers it. The shader warm-up now advances
  every frame instead of only when the game asks for a pipeline (before, U1's entries waited
  about 4.8 s; not re-measured). In a 4K DLAA A/B in U3 the gain
  (+2.8 FPS on average) was within run-to-run noise.
- **Streaming reads run in parallel again.** Reads at an offset on one file no longer wait for
  each other, so the streaming threads load level data at the same time.
- **No long stalls from the log.** The game log is buffered and written every 250 ms. Before, each
  line was written under a lock, and the graphics thread once waited 548 ms for it in a level load.
  If a thread dies holding that lock, some last log lines can be lost.
- **Fixed: "Apply and restart" in the in-game menu now works.** The button was always disabled. It
  now relaunches the game in the same launcher process and log, with the new settings (for
  example a new resolution). Not tried live; if the restarted game never shows, the old settings
  are not restored.
- **Fixed: menu keys no longer stick or repeat.** Closing the menu left keys held, a key held
  while opening it repeated Enter or Escape, and saving on close froze the frame. Input is now
  cleared on close, key repeat is ignored in dialogs, and the save runs in the background.
- **Your saves are backed up on every launch.** Before the game starts, `savedata` is copied to
  `analysis\uncharted-user\save-backups\<date-time>`. The newest 5 are kept, and nothing is copied
  if nothing changed. Killing the game with STOP during an autosave could leave a broken save with
  nothing to restore.
- **Hardening from the second audit.** Save data files (`param.bin`, `icon0.png`) are written
  atomically and save titles with path separators are refused; threads free their memory and file
  handles close safely while another thread reads; memory ranges near the end of the pool are
  bounds-checked; movie slots 17 and up recycle the oldest one; a corrupt `param.sfo` is bounded;
  a locked game ini is no longer overwritten with the shared one; linking a module without
  `DT_INIT` stops with an error; the DLSS SDK download is pinned to one commit and hash-checked;
  the package stops on a DLL with no known license owner; the export no longer lists two deleted
  tests.

- **Fixed: the DLSS check no longer calls a genuine `nvngx_dlss.dll` unsigned when the folder name
  has an apostrophe.** In a folder such as `C:\Users\D'Angelo\...` the signature check failed to
  run, so the launcher rejected a genuine NVIDIA DLL. The path is now passed outside the check's
  script text.
- **Fixed: a movie of a different size or format no longer corrupts memory.** The video decoder
  kept the picture conversion it built for the first movie, so a decoder reused for a movie with
  another size or pixel format wrote rows past the end of its frame buffer. The conversion is now
  rebuilt when the size or format changes, and a failed buffer allocation now stops with a clear error instead of corrupting memory.
- **Fixed: a game build with different bytes at a patch site no longer stops the start.** A patch
  whose bytes did not match was refused with an error for some patches; the game now starts
  without that patch and the log says "Patch '...' skipped: unexpected bytes at ...".
- **Fixed: a mod path with `..` between backslashes could reach files outside the user folder.**
  Such a path is now refused. Only matters if you use mods.

- **Fixed: a failed update no longer loses the installed version when your antivirus holds a
  file.** If an update failed and the antivirus kept one of the new files locked for 15 seconds,
  the installer gave up restoring the old files, and the next launcher start deleted the folder
  that held them (including your `nvngx_dlss.dll`). Each item is now restored on its own. If one
  cannot be restored, the old files stay in `update\old` with a `RESTORE-INCOMPLETE` file that
  lists them, so you can copy them back. Starting another update still clears that folder.
- **Diagnostic modes no longer freeze or stall the game.** With `GOW3_PROFILE_STALLS=1` the
  profiler could deadlock the game while it recorded a sample, and `GOW3_RETEXTURE_FILE` looked
  for its trigger file on every game submission (25-50 ms stalls, as in the entry below). The
  sample is now stored after the game thread resumes, and the trigger file is checked by the
  background thread. Only matters if you set these variables.

- **Fixed: your upscaler choice is no longer overwritten when it cannot run.** If DLSS could not
  start (no `nvngx_dlss.dll`, or a driver without it) or FSR 4 is not supported by the graphics
  card, the game used FSR 3.1 for that launch, and closing the in-game menu saved FSR 3.1 as your
  setting. The menu now keeps what you chose; the fallback lasts only for that launch.
- **Fixed: settings no longer go back to defaults after a crash or a kill.** The launcher, the
  start script and the in-game menu wrote the settings file in place, so ending the game or
  launcher at the wrong moment (or both writing at once) could leave it empty or cut off. They
  now write a temporary file and swap it in.
- **Fixed: a possible hang in timers the game waits for.** A timer request that arrived at the
  wrong instant could be dropped, so a game thread waiting for that timer never woke. Not shown to
  happen in Uncharted and not reproduced; the request is now kept.
- **Fixed: the game no longer fails to start with an update folder from another version.** After
  switching copies (for example from the US 1.02 update to the Europe CUSA02344 copy), the old
  update folder crashed the start with a ValueError. The game now starts without the update and
  the log says "Update not used (...)".

- **Fewer hitches from the port's diagnostic files.** With the performance log on (the default),
  the graphics threads looked on disk for four "trigger" files (frame capture, frame dump,
  readback and DMA switches) on every frame or game submission. While Windows was busy with
  files, one look could hold the frame for 25-50 ms (measured in Uncharted 3). One background
  thread now looks for them four times a second; writing a trigger file works as before.

- **New Ultra Quality upscaling mode, between DLAA and Quality.** The scene is drawn at 71% of
  the width and height (2742x1542 for 4K), and DLSS or FSR rebuild the full picture. In
  Uncharted 3 at 4K with DLSS model M: about 24% faster than DLAA (68.6 -> 85.2 FPS in the bar
  fight); Quality is about 31% faster. Your saved mode is unchanged.
- **The DLSS model's description now says what model M costs.** It said M costs a little more
  than K. With DLAA at 4K, M takes 7.9 ms of every frame and K 2.2 ms (RTX 5070), so Uncharted 3
  runs at about 68 FPS with M and about 100 FPS with K. NVIDIA's own choice is K for DLAA,
  Quality and Balanced, and M for Performance mode. In Performance mode the two are close (2.9 and
  2.0 ms).

- **Uncharted 3 runs a little faster with multithreaded rendering.** Every second, the GPU
  thread copied 2.5 GB of the small data blocks the game's GPU jobs read (constants). Half of
  them it had already copied, and almost none of those copies were reused. It now reuses a copy until something
  could have changed those bytes: a GPU write over them, or a new submission from the game. At
  4K with DLAA in the bar fight: +1.4% with DLSS model M, +6.6% with model K (about 102 FPS).

- **Bug reports on this fork:** the fork's GitHub page now has Issues, with a form that asks for
  what the crash needs to be traced (boot.log, game version, graphics card, launcher settings).

- **Fixed: a 15.8 GB texture could crash the game with "Device lost" after a long session**
  (upstream #57, Drake's Fortune after 26 minutes on an 8 GB card). A broken texture description
  (6,755 layers) got through the check for impossible textures, because its size was counted in
  a number that overflows. The size no longer overflows, so such textures are skipped as the
  others are.

- **Cutscenes at full speed again at 4K.** Since 0.4.0's sharper cutscenes, a 4K render played the
  movies at about 10 FPS (Uncharted 3's first cutscene): the sharper scale ran on one CPU thread
  and took 18 ms a frame. It now runs on four threads (6 ms), and the cutscene plays at the frame
  limit with the same sharpness.
- **BilalReloaded's 0.4.0 merged** (its notes are in the "upstream 0.4.0" section below). What
  it brings to this fork:
  - **No more freezes before some cutscenes** (#52).
  - **Drake's Fortune's enemy grenade launchers no longer blow up on Drake** (#6).
  - **Drake's Fortune's Infinite ammo keeps the magazine full.**
  
  What stays as it was in the fork:
  - **Upscaling:** the fork's FSR/DLSS quality modes, set in the Anti-aliasing option, already
    draw the scene smaller and keep the HUD sharp, so the launcher and the in-game menu are
    unchanged. Upstream's Upscaling option is not taken.
  - **Multithreaded rendering** stays on for Uncharted 3. The fork fixed its freeze (#51) the
    same way upstream did, and upstream's switch that turns it off for Uncharted 3 is not taken.
    Drake's Fortune keeps it on too.

## 21 October 2026

- Uncharted 3: busy areas with many effects (e.g. the Cartagena market in chapter 2) run about 5% faster
  (about +3 FPS at 60-65 FPS): the GPU command thread no longer waits for the draw recording thread
  before reading resource tables when nothing it reads is still being written.

## 19 October 2026 (fork 0.4.0, Expedition Edition)

This fork ([espindoIa](https://github.com/espindoIa/Uncharted-The-Nathan-Drake-Collection-PC-port))
adds the work below on top of 0.3.0, and has 0.3.1 (next section) merged in. Where both did the
same thing, one was kept:

- **US version (CUSA02320) with update 1.02 tested:** all three games start and play with it, and
  every fix of the fork applies (each patch and hook found its bytes: the 1.02 executables are the
  same as Europe's CUSA02344). The upstream 0.3.1 notes list this version as stopping at start
  (#29, #43, #48); that stop did not happen with this copy, so its cause there is still unknown.
  The launcher and `probe_uncharted.py` look for `game\CUSA02320` first, then `game\CUSA02344`.
- **The launcher has its own look, "Expedition Edition":** night-blue panels with a signal-flare
  orange, the game's art as a two-tone chart with a map grid and a marked route, Bahnschrift
  headings, a compass icon (`tools/make_launcher_icon.py`) and the fork's name in the launcher and
  game window titles. The header links to this fork; the credits on the Advanced page name
  BilalReloaded's port it builds on. Launcher updates come from this fork's releases now (none
  yet), no longer the upstream port's, whose packages would replace the fork's work.
- **Uncharted 2: no more regular stutter at the GPU's limit.** About every fifth frame was
  finished together with the next one and stayed on screen 30+ ms: a frame waiting for GPU
  results of earlier work was prepared only once they were back, behind the next frame's work.
  It is now prepared as soon as the game flips it. 4K DLAA, VSync on: on-screen intervals over
  25 ms 18% -> 0.4%, worst 1% 31 -> 21 ms, and ~62 -> ~66 FPS. Uncharted 3 unchanged.
- **Smoother frame pacing.** The game made its frames evenly, but the port held each one until
  GPU results for earlier work were back before showing it, so frames reached the screen in
  bursts: a 38 ms gap, then two at once. Frames now go to the screen as soon as they are drawn
  (the game still sees each flip in the old order). Uncharted 2 at 4K DLAA, VSync on, same 65 FPS:
  worst 1% of the port's frame intervals 39 -> 24 ms (1% low 25 -> 40 FPS); on screen (as the
  driver reports it), frames arriving less than 5 ms after the previous one 5.8% -> 0.5%.
  Uncharted 3: same FPS, port intervals p99 29 -> 25 ms, on screen unchanged (already even).
- **Uncharted 3 no longer freezes or crashes seconds into gameplay with multithreaded
  rendering** (reported at 4K, 120 FPS, DLAA). Fence labels, `WRITE_DATA` writes and the flip
  interrupt could reach the game before an earlier end-of-frame fence that waits for GPU ray-cast
  results: the game then waited for events in the wrong order (a freeze; closing the frozen window
  looked like a crash) or reused a command buffer still being read ("Invalid packet"). They now
  queue behind that fence. 8 minutes of play at those settings without a problem, 66-78 FPS
  against 52 without multithreading.
- **Three more multithreaded freezes fixed** (Uncharted 3, found by restarting the game into
  gameplay again and again): a fence written by the Vulkan recording thread into a protected page,
  and two cases of a GPU interrupt delivered on a thread that already held the lock its handler
  needed (the flip presents a frame). Fences from that thread now go through the backing view;
  interrupts raised inside a handler or a GPU-side fault wait for the thread's next safe point.
- **Faster when the game writes into memory the GPU just wrote** (Uncharted 3: ~30 times a
  second): the game's own thread now waits for that GPU work, not the thread that feeds the GPU.
  Uncharted 3, chapter 3 street, 4K, DLAA model M, alternating 15 s windows in one session:
  55-56 FPS against 49-50 without it (p50 17.4 against 19.6 ms). Nested compute command
  buffers no longer wait for the draw recording thread either: +2% there (two rounds: 55.5 and
  56.4 FPS against 54.8 and 54.9), p99 ~27 -> ~24 ms.
- **Where the frame time goes at 4K with DLAA.** At 4K DLAA all three games are limited by the
  GPU, and much of it is DLSS itself: model M costs 7.8 ms a frame there, model K 2.2 ms
  (Uncharted 3: 54 FPS with M, 61 with K). Drake's Fortune and Uncharted 2 run the same as before
  at 4K DLAA M (94 and 64 FPS, GPU 93-94% busy); with model K or the DLSS Quality mode Uncharted 3
  is then held by the port's draw recording thread at ~61 FPS (next to work on).
- **Scripted play** (`tools/play.py`): start a game, press buttons and move the sticks for a set
  time, get the current screen as a PNG after each step (taken inside the port, window focus not
  needed), record and replay routes, compare screenshots, measure frame times.
- **Sharper cutscenes above 1080p.** The cutscenes are 1080p movies (DLSS and FSR never touch
  them: the game draws each movie frame as one picture); their scale to the render resolution now
  uses Lanczos instead of bicubic (`GOW3_MOVIE_SCALER=bicubic` for the old filter).
- **The log says why the game window closed** ("Window: closing ..."), so a window closed by
  hand or by Alt+F4 is no longer mistaken for a crash.

- **Anti-aliasing and upscaling in one place.** The 0.3.0 Anti-aliasing option (launcher Display
  tab, in-game menu Image tab, switches live) now also offers FSR and DLSS **quality modes**
  (Quality, Balanced, Performance, Ultra Performance: the scene is drawn smaller and rebuilt at the
  render resolution) besides Native AA / DLAA, and the **DLSS model** J, K, L or M (K, the fastest,
  by default). The camera for it comes from the scene shaders, with motion vectors for characters
  and plants, so moving characters leave no trails; 0.3.0's camera from the motion blur
  (`UNCHARTED_MOTION_SOURCE=velocity`) stays available, Native AA only.
- **Cutscenes keep the frame rate.** Above 60 FPS both versions fix the movies' speed: 0.3.0 by
  dropping the whole game to 60 Hz while a movie plays, this fork by pacing the movie player by the
  game's clock (Uncharted 3 at 120 FPS: 1.00x, was 2.0x), so cutscenes keep 120 FPS. The 60 Hz
  switch stays as the fallback for a game build the fork's patch does not recognise (checked when
  the first movie plays: checked at start, it was always on, and cutscenes dropped to ~13 FPS).
- **NVIDIA's nvngx_dlss.dll is not in the package** (its license does not fit the port's GPL): the
  launcher opens NVIDIA's download page and adds the file after checking NVIDIA's signature. DLSS
  is then listed under Anti-aliasing.
- **Multithreaded rendering is on by default** in all three games (0.3.0 kept it off in Drake's
  Fortune for flickering grey guns, GitHub #23: turn it off there if they show up): its hang after about 10 minutes is fixed, and on a 6-core Ryzen it gives +27% in
  the Uncharted 3 bar fight (62.8 -> 80 FPS at 1080p). Uncharted 2's train flashing white with it
  is fixed, and so are three rare ordering problems (among them Uncharted 2's crosshair centre).
- **Faster GPU command processing.** Memory-mapping checks and lock bookkeeping are cheaper
  (Uncharted 3 bar fight: frame time -4.4%); the register scan per draw and repeated small
  constant-buffer copies are gone (Drake's Fortune uncapped: 99 -> 119 FPS).
- **Idle game threads:** 0.3.0's sleep patches are kept (launcher Performance tab); a worker
  now waits at most 1 ms instead of 10 for a job queued just as it went to sleep. 0.3.0's bounded
  wait replaces the fork's own fix of the `m_gfxEopTick` assert.
- **Hook stubs within reach.** The runtime's code patches put their helper code within 2 GB of the
  game code wherever the game is loaded (a packaged Uncharted 3 skipped the cutscene fix before).
## 19 October 2026 (upstream 0.4.0, BilalReloaded)

- **No more freeze before some cutscenes** (#52): before a cutscene the games remove ~100 small
  blocks of memory, and for each one the port rebuilt the whole surrounding mapping (about 23 ms
  each on Windows, far more under Proton): Drake's Fortune froze 1.6 s, the Steam Deck up to 33 s
  in Uncharted 3's museum. Memory is now mapped in 2 MB pieces, so a removal only touches the
  pieces around it: the same cutscene went from a 1650 ms frame to ~200 ms.
- **Uncharted 3 no longer freezes for good with multithreaded rendering** (#51): a fence write on
  the Vulkan recording thread could fault while the GPU command thread waited for that thread.
  It now never faults. Multithreaded rendering stays off for Uncharted 3 until players confirm
  it (`UNCHARTED_U3_MULTITHREADED_TEST=1` keeps the launcher's choice).
- **Drake's Fortune: enemy grenade launchers no longer blow up on Drake** (#6): from the second
  shot on, the grenade exploded right in front of him. The shot started from a frame-old copy of
  data the GPU writes for the game (a copy shader whose results it reads, the cause of
  Uncharted 2's floating NPCs too); that data now reaches the game before it is used, as in
  Uncharted 2 and 3. No frame-rate cost measured.
- **Drake's Fortune: Infinite ammo keeps the magazine full** (Cheats tab): the game's own switch
  only spared the reserve when reloading; each shot still emptied the magazine.

From kuraeow (Steam Deck), merged:

- **Real upscaling** (#50, asked for in #45): Display -> **Upscaling**: FSR 1, FSR 3.1, FSR 4 or
  DLSS, with **Quality**, Balanced, Performance or Ultra Performance. The game draws below your
  resolution and each frame is upscaled to it before the HUD and menus are drawn, which stay
  sharp. **Native AA / DLAA** (no upscaling) stays as before. Upscaling saves GPU time: it helps
  at 4K or on weaker graphics cards, much less where the processor limits the frame rate.
  Moving characters can smear below Quality (the motion vectors come from the camera only).
  The **Low** preset is now 1080p with FSR 1 Performance (HUD at 1080p). Drake's Fortune above
  1620p draws at 1620p and is upscaled with the HUD at full size.

## 19 October 2026 (0.3.1)

- **Choose the game update in the launcher** (#32): Launcher -> Advanced -> GAME UPDATE.
  **Choose update folder**: your extracted update 1.02, wherever it is and whatever it is called
  (nothing is copied; the launcher remembers it). **Install from .pkg**: the update's `.pkg` file,
  unpacked next to the game folder under the right name. Both check that it is an update for your
  copy of the game (same region) and that nothing the port needs is missing (the update's
  `data.psarc` files, for example).
- **A damaged update is named instead of crashing:** with an update whose game archives are
  incomplete or damaged, all three games stop at start with "files.json -> not found" and
  `ASSERTION: root` (ingame-shaders.cpp). Before a game starts, the launcher now checks the update's
  archives (complete, readable, shader list inside) and says which file is damaged. The port still
  ships no game files: you need your own update PKG. Uses PkgTool from LibOrbisPkg (LGPL-3.0).
  Not fixed yet: the same stop with the US version CUSA02320 (#29, #43, #48), whose update files
  are intact. Until then, renaming its `-patch` folder starts the games as 1.00.
- **Shortcuts on a moved Desktop** (#39): Desktop shortcut and Game shortcuts failed with
  "DirectoryNotFound" when Windows keeps the Desktop elsewhere (OneDrive, a renamed or redirected
  folder). The launcher now asks Windows where it is.

- **"exception 0x20474343" says what failed** (#47): an error nothing caught on the GPU command
  thread ended the game with only that code. The log (and the launcher's crash message) now names
  it, for example "out of memory on the GPU command thread". The crash guard of 0.3.0 no longer
  needs memory to be available: when Windows refuses its snapshot, that readback lands as before.
- **A graphics pipeline the driver refuses no longer stops the game** (#40, AMD Radeon RX 9070 XT):
  "Failed to create graphics pipeline: ErrorInitializationFailed" ended every game at start. The
  pipeline is now created again without the pipeline cache; refused again, only the draws that need
  it are skipped and the log names it ("GPU: the driver refused graphics pipeline ...").

From SuperMedo/uncharted_pc (thanks to kj2223 and hugouchoasborges), ported:

- **Sun shadows no longer flicker with TAA, FSR or DLSS/DLAA** (#261, kj2223). The sun shadow pass
  (the same shader in all three games) rebuilt each pixel's position from the scene depth without
  the anti-aliasing's sub-pixel offset, so its shadows moved against the geometry every frame. It
  now gets the same offset as the scene.
- **Steadier anti-aliasing** (#261, kj2223): Uncharted draws twice into the picture it shows, and
  the port started a new frame on both. The second start threw away the anti-aliasing's history
  every frame and skipped every other sub-pixel position. It is now one frame start per frame
  (measured: 600 starts per 10 s at 60 FPS, was per 5 s).
- **Steam Deck and other Linux AMD/Intel GPUs** (#94, hugouchoasborges): memory binding goes to a
  GPU queue that supports it (on Steam Deck the graphics queue does not), and large texture
  conversions are split to stay within the GPU's limit (65,535 work groups on some GPUs). On
  NVIDIA and AMD on Windows nothing changes.

From kuraeow (Steam Deck), merged:

- **16:10 aspect ratio** (#35): 720p becomes 1280x800 (the Steam Deck's screen); the games show
  more at the top and bottom. Resolution buttons are labelled for the chosen ratio, with the exact
  render size below.
- **Cutscene movies keep their shape** at sizes that aren't 16:9 (#36): black bars instead of a
  stretched picture, at 16:10 and ultrawide.
- **Update 1.02 merged into the game folder** (#37) is recognised even when its param.sfo still
  says 1.00 (no more "install update 1.02" warning, cheats work).
- **Steam Deck / Proton**: one set of launcher options for every Steam shortcut (#38).

## 19 October 2026 (0.3.0)

- **Random crashes after a while, "exit code 3"** (#30, #24, #27): GPU results reach the game's
  memory a little late, up to a frame or two after the game was told the GPU had finished. When
  the game reused that memory in between (an area unloading, a cutscene starting), the late copy
  overwrote the game's new data: crashes in the game's memory allocator, a function pointer turned
  to 0, and similar. Late GPU data now never overwrites a 64-byte block the game changed in the
  meantime. In 51 minutes of Uncharted 3 it stepped in 38 times, once exactly where the game had
  crashed before. Logged as "Readback guard: kept ..."; `GOW3_READBACK_GUARD=0` turns it off.
- **Uncharted 3: blurry view when aiming** (#16): the aiming depth of field read its focus
  distance from a one-pixel image the GPU renders, which was only copied back to the game in
  Uncharted 2. It stayed at 0 m and everything behind Nate was blurred. Uncharted 3 now gets it
  too; the focus follows what you aim at.
- **Fewer shader hitches**: compute shaders found in the game's command buffers are compiled in
  the background before the GPU needs them, instead of all at once on the frame that first uses
  them (`GOW3_COMPUTE_PRECOMPILE=0` turns it off).
- **Cheats** (#26): the launcher's new **Cheats** tab: Invincible, Infinite ammo, No reload
  (Uncharted 2 and 3), Infinite grenades and One-hit kills (Uncharted 3). They switch on the games'
  own developer options; they need update 1.02. In Uncharted 1 and 2 the infinite ammo option lives
  in the games' settings system, which resets it every frame, so the places that read it are
  changed instead.
- **Ultrawide shows more at the sides** (#20): at 21:9 and 32:9 the three games kept the 16:9
  picture's width of view and cut off the top and bottom instead. They now keep the 16:9 view top
  to bottom and widen it to the sides, culling included (nothing missing at the screen edges);
  checked in all three games at 3440x1440 and in Uncharted 2 at 5120x1440. 16:9 is unchanged.

- **Start a game directly** (#25): Launcher -> Advanced -> **Game shortcuts** puts one desktop
  shortcut per game; each starts its game with the launcher's settings, without the launcher
  window. From a command prompt: `"Uncharted Launcher.exe" --play u1` (or `u2`, `u3`). A
  double-click on a game's card in the launcher plays it too.
- **Launcher: buttons on the Advanced page were hidden** (log folder, save folder, shortcuts,
  shader cache, reset, check for updates): the page had grown taller than the window. Pages
  that don't fit now scroll, and they are grouped as Shortcuts, Folders and Troubleshooting.
- **Launcher: easier to use.** More room for the settings (a smaller header; the window opens
  taller when the screen allows), plainer descriptions, recommended choices marked with a star,
  and Quick Setup marks the preset that suits your graphics card ("Recommended").
- **Warning without update 1.02** (#24): most of the port's game fixes, crash fixes among them,
  are written for update 1.02 and are skipped on 1.00 (the log listed them as "NOT applied"
  without saying why). The launcher now says so once when you press Play, and the log names the
  game version and where the update goes.
- **Patreon** link in the launcher header.
- **Crash messages say what crashed.** "Exit code 3" is the port's general crash code; the launcher
  now shows the crash's details from the log (what failed and where) and asks for boot.log, instead
  of only the exit code (#24, #27).
- **Freeze reports.** When a game shows no new frame for 30 seconds, the start script records where
  every thread is stuck (hang-<time>.txt in the log folder, a few KB) and the launcher points to it.
  Please attach it with boot.log to a GitHub issue: every freeze can now be traced to its cause.
  A fault inside the port's own fault handling now stops with a report instead of freezing.
- **Freeze in any game** (since 0.2.0, seen in Uncharted 3): when a shader's resource table held a
  garbage pointer, the 0.2.0 fix for the rooftop freeze looked that address up in its page table
  without a range check, which faulted inside the fault handler, endlessly. It now only looks up
  game addresses; such loads are skipped as before 0.2.0.
- **Crash after a long session** (seen in Uncharted 2 after 38 minutes, `Uncharted.exe+0x11474b`):
  when the texture cache's image table grew while a depth buffer got its stencil part, the code
  went on using the old location of the depth image. Fixed there and in one other place with the
  same mistake.

From the work on a ROG Zephyrus G14 laptop (Radeon 780M and RTX 5060 Laptop GPU), merged:

- **Graphics cards with 6-8 GB: no more texture thrashing.** The texture cache started throwing
  textures out at about 40% of the card's memory, so on 6-8 GB cards it evicted hundreds of
  textures every few seconds and reloaded them (75-150 ms hitches about every 15 s on an 8 GB RTX
  5060). It now goes by the driver's memory budget, as on integrated GPUs. The crashes in #24 (GTX
  1660 SUPER, 6 GB) all came while it was evicting.
- **Uncharted 3 could stop responding when a level started** (a guest write looping on a page a
  GPU readback still protected): the readback is now landed at once. A second guard beside the
  rooftop fix of 0.2.0.
- **Update 1.02 from exFAT and FAT32 drives** (USB sticks): those drives hold no links, so the
  update was skipped and the game ran as 1.00. The runtime now lays the update over the game itself.
- **"Error 1450" at start on PCs with 16 GB of RAM**: the game's memory is now committed as it is
  used instead of all at once.
- **Memory corruption after partly freeing game memory** (flexible memory split): could zero live
  game data on any PC, a candidate for random crashes.
- **Multithreaded rendering**: readbacks could be landed twice (heap corruption) in Uncharted 3;
  fixed. Uncharted 3 ran 24% faster with it on the 780M.
- **Drake's Fortune compiles shaders in the background** again (it froze 1-2.4 s in each new area;
  now about 120 ms at worst). UNCHARTED_U1_SYNC_SHADERS=1 restores the old way.
- **RTX laptops: fewer hitches** (render targets no longer get a driver allocation each), **AMD
  GPUs**: fewer allocation stalls at level start and a cheaper second-queue fetch.

## 18 October 2026 (0.2.3)

- **Uncharted 1: guns flickering flat grey** (#23). With multithreaded rendering on, Nate's rifle
  and pistol (and at times other objects) were drawn flat grey on some frames. Drake's Fortune now
  always runs without multithreaded rendering: it gained nothing from it there.
- **Uncharted 2 crash `m_lookAt.X() == m_lookAt.X()`** (#24): the point Nate looks at could become
  invalid (NaN) and the game stopped at its own check, as Uncharted 3 did in #13. Uncharted 2 now
  resets that point to where the player looks instead, as the Uncharted 3 fix does.
- **Frame captures** (diagnostics): a capture requested during play could end the game when a
  shader had a buffer at address 0; such buffers are now skipped.

## 18 October 2026 (0.2.2)

- **Windows 10: all three games crashed at start on some PCs** (#22): the games' job code runs on
  PS4 fibers with their own stacks, and some Windows 10 builds end the process when an exception
  resumes on a stack the thread doesn't have registered. The port now registers the stack in use
  before resuming.
- **Uncharted 3 crash in Syria** (#21, `PatchImageSharp`): a damaged image description while a
  level loads made the shader translator stop the game; that one write is now skipped.
- **All the Collection's languages.** Launcher -> Advanced -> **Language**: the language the PS4
  would be set to, from the 22 the games have text for (English US/UK, French, German, Italian,
  Spanish and Latin American Spanish, Portuguese and Brazilian Portuguese, Dutch, Danish,
  Norwegian, Swedish, Finnish, Polish, Czech, Greek, Russian, Turkish, Japanese, Korean,
  Traditional Chinese). Each game's main menu then offers it next to English: press **Square**
  to switch its menus and subtitles. Spoken language depends on your copy of the game: the list
  marks the languages it has voices for (the European CUSA02344 has English, Polish, Russian and
  Turkish). Before, the port always reported English, so only English was offered.
- **Unlock all chapters.** Launcher -> Advanced -> **Unlock all chapters**: every chapter in each
  game's Chapters menu (Drake's Fortune 1-22, Among Thieves 1-26, Drake's Deception 1-22) and the
  Brutal difficulty from the start, without finishing the games first. It turns on the
  Collection's own "unlock by default" switches, so saves and progress are untouched. Off by
  default; needs update 1.02.
- **Launcher errors are shown and saved** (`analysis\uncharted-user\launcher-error.log`); an error
  at start used to close it silently.
- **Long sessions:** a diagnostics list that grew with every GPU image and buffer made in a session
  (never shrunk, searched on every free) is now kept only with the GPU fault diagnostics on.

## 18 October 2026 (0.2.1)

- **NVIDIA DLAA and AMD FSR 4 Native AA are in the package.** The 0.2.0 build could not fetch
  their files (tools/fetch_aa_files.sh copied the Vulkan headers from `//clang64` on the build
  runner, where the MSYS2 root is `/`), so the menus offered only TAA and FSR 3.1.

## 18 October 2026 (0.2.0)

- **Smooth frame pacing above 60 FPS with VSync (all three games).** With a frame rate limit at or
  above the monitor's refresh rate and VSync on, the game made frames faster than the monitor
  shows them; they queued up behind VSync and the game was held back in bursts: two or three
  quick frames, then a long one. Now the game follows the monitor (its refresh rate, up to
  120 Hz), as it follows a 60 Hz screen: on a 75 Hz monitor at the 120 setting, Uncharted 3's
  rooftops and Uncharted 2 run at an even 75 FPS, 1% lows 32 -> 66 FPS (Uncharted 2: 22 -> 67),
  no frame over 20 ms (28% before). It switches live with the in-game VSync and limit; monitors
  above 120 Hz, VSync off and lower limits keep the earlier pacing. New frame rate limits 75, 90
  and 100 (launcher and in-game menu) for those monitors and variable refresh rate screens.
- **Uncharted 3: no freeze on the Cartagena rooftops (chapter 2).** The game froze about 30
  seconds into the rooftop chase at 120 FPS: it cleared a field of its next ray-cast batch in
  memory still guarded for a GPU result on its way back; the port's fault handler let the write
  fault again and again, and the result it waited for was never collected because the GPU
  thread was idle waiting for the game. Such a write now collects the result at once (or waits
  for the GPU when it is still running). Shorter versions of the same loop, about 30 a second
  in that area, were small hitches in all three games. While the GPU thread waits for the GPU
  at the end of a frame it now also collects finished results (fewer long frames at high
  frame rates).
- **Cutscenes play at normal speed with the frame rate above 60.** The Collection's cutscenes are
  prerecorded movies that show one picture per screen refresh; with the frame rate unlocked
  (120) they ran fast and out of sync with their sound (reported in Uncharted 3). While a movie
  plays the game now gets 60 Hz refreshes; gameplay keeps the chosen frame rate. (The same fix
  as in SuperMedo's Uncharted port.)
- **Anti-aliasing: TAA, AMD FSR 3.1 and FSR 4 Native AA, NVIDIA DLAA (all three games).** In-game
  menu -> Image -> Anti-aliasing, or launcher -> Display -> Anti-aliasing: Off (the game's own, as
  before), TAA, FSR 3.1, FSR 4 (listed when the GPU can run its 8-bit model; tested on an RTX 5080)
  or DLAA (NVIDIA RTX), with sharpening for FSR and DLAA. It works at any render resolution, ultrawide
  included, and switches at once from the menu. Jagged and shimmering edges (roof tiles, railings,
  foliage) are much calmer: edge flicker about halved in Uncharted 2, a third of what it was in
  Uncharted 3 (frame-to-frame measurements). The camera's motion comes from the games' own
  motion-blur velocity pass, an idea from SuperMedo's Uncharted port. Turning the camera leaves no
  trails; TAA softens the picture slightly. DLAA calms edges the most (Uncharted 2: a third of the
  flicker). Cost on an RTX 5080 at 4K in Uncharted 2: TAA and FSR 3.1 kept the 120 FPS cap, DLAA
  ran at about 84 FPS (NVIDIA's model, about 3.6 ms a frame at 4K) and kept 120 FPS at 1440p.
  The package carries NVIDIA's `nvngx_dlss.dll` (DLSS SDK 310.9.16) and the FSR 4 native model
  (tools/fetch_aa_files.sh fetches them for builds). UNCHARTED_TEMPORAL_AA=0 turns it all off.
- **Ultrawide monitors (GitHub #7).** Launcher -> Display -> Aspect ratio: 16:9, 21:9 or 32:9. The
  render resolution's height is kept and the picture widened (1080p at 21:9 is 2560x1080, 1440p
  is 3440x1440); all three games show more to the sides with nothing stretched, the HUD and menus
  keep their layout, aiming stays centred. Use Borderless or Fullscreen on the ultrawide monitor.
  Drake's Fortune, which runs out of GPU memory above about 1620p, steps down in the same shape.
- **Draw distance options (Uncharted 2 and 3, experimental).** In-game menu -> Image -> Draw
  distance: object fade distance (up to 4x the game's own, which fades objects and details in at
  0.58 of their authored distance), sun shadow distance and lamp/torch shadow distance (up to 3x
  the game's 40 m). Applied live; higher values cost some FPS.
- **Fewer hitches after loading (shader compiles).** On NVIDIA GPUs the cached shaders were only
  prepared when a level first needed them, and compute shaders were translated in the middle of
  the frame: Uncharted 3 hitched for 90-170 ms several times right after a load. The shader cache
  now loads in the background while the title screen shows (2,400+ pipelines in 7-16 s), so none
  are compiled in play. GOW3_ON_DEMAND_PIPELINES=1 keeps the old start.
- **In-game menu: your own shortcut (GitHub #15, #19).** Controls -> Menu shortcut: LT + right
  stick click (default), View + Menu together (no clash with the sniper zoom), none, or Custom:
  select "Custom menu shortcut" and hold any two controller buttons (or one the game does not
  use, such as Guide or a paddle). "Menu key (keyboard)" changes Insert to any key. The menu's
  title shows the current shortcuts.
- **Uncharted 3: no crash climbing the fort in chapter 8 (GitHub #13).** When the point Nate looks
  at became invalid (NaN), the game stopped (`m_lookAt.X() == m_lookAt.X()` in UpdateFocus); now
  it resets that point the way the game already does when it is far off.
- **Fewer crashes in all three games (GitHub #11, #12, #17).** The most reported crash, the game's
  check that the GPU finished frame N-3 (`m_gfxEopTick` in FrameBegin), came after short stalls on
  slower or busier PCs: the game's GPU event thread had not marked that frame done yet. The game
  now waits for it (a brief hitch at most) instead of stopping. UNCHARTED_FRAME_WAIT=0 keeps the
  original check.
- **The aim sensitivity sliders go higher in all three games.** Options -> Controls -> Aiming ->
  Sensitivity now reaches five times its old maximum in Uncharted 1 and 2 (a notch is half the old
  maximum, the second notch the old right end) and four times in Uncharted 3 (the left half keeps
  its fine steps, the right half goes up to 4x), for fast aiming with the mouse or a controller.
  (The games' own range checks are lifted; UNCHARTED_AIM_RANGE=0 keeps the original sliders.)
- **Controls in the in-game menu.** Insert (or LT + right stick click) -> Controls: rebind every
  action for keyboard, mouse and controller (select a slot, press the new key or button), mouse on
  and off, mouse sensitivity, invert, button icons, a controller camera speed (Uncharted 2 and 3,
  not yet in every camera) and Reset. Changes apply at once and are kept in the game's settings
  file, which the launcher's Controls tab now reads and writes too.
- **Keyboard and mouse button icons in all three games.** While you play with the keyboard and
  mouse, every button icon the games draw in their text shows your keys and mouse buttons instead
  of the PlayStation buttons: tutorial hints ("Press SPACE to jump"), menu legends (SPACE Select,
  CTRL Back, ESC Resume), the sticks (WASD, the mouse) and the d-pad (1-4). The icons are drawn
  from your own bindings, so remapped keys show up too. The prompts the games draw in the world
  (the Triangle beside a chest, the round Square/Circle/Cross buttons, Uncharted 3's L1/R1/L2/R2)
  change too: the same button picture with your key in place of the PlayStation symbol, or a mouse
  for the mouse buttons. Touch a controller and its icons come back at once; use the keyboard
  again and they switch back. Xbox and other controllers get their own buttons (A B X Y in their
  colours, LB RB LT RT, Menu, View); PlayStation controllers keep the game's own icons. Launcher:
  Controls -> Button icons: Auto (the device you use) / Keyboard and mouse / PlayStation / Xbox.
- **Launcher: controller bindings with button icons.** The Controls tab shows each action's
  controller button as an Xbox or PlayStation icon (Auto: the controller you have connected).
  Click a controller slot, then press a button on the controller to rebind it; right-click clears
  it; a button already in use swaps with the old one. The triggers and sticks stay Aim, Fire, Move
  and Look. The in-game icons follow your bindings. The launcher window can now be resized.
- **Uncharted 2 and 3: raw mouse look.** The mouse turns the camera directly (1:1, no stick
  emulation, no acceleration or dead zone), at the same speed horizontally and vertically;
  Sensitivity and Invert Y in the launcher apply. Uncharted 1 still uses the mouse as a stick.
- **Low-end PCs: the games no longer take every core of a 4-6 core CPU.** The engine's six job
  threads spin while waiting for work (on the PS4 each has its own core). On 4 cores they used 390%
  of 400% CPU and starved the GPU thread: Uncharted 3's title screen ran at 19-27 FPS on an RTX
  5080, its first level at 5-7 FPS on an integrated GPU. The engine has its own sleep mode for idle
  job threads (a job wakes them); it is now always on (launcher: Performance -> Idle game threads:
  Sleep / Spin). 4 cores: 60 FPS at the title and in Uncharted 1 and 3, about 1.5 cores used;
  integrated GPU: 27 FPS at 720p (was 5-7). 16-core desktop: Uncharted 3 uses about 2.3 cores
  instead of 6.4, and Uncharted 2's heaviest view runs 76.5 -> 78 FPS (uncapped).
- **540p render resolution** (launcher, Display) for integrated GPUs, with FSR 1 to the screen:
  tested in all three games; Uncharted 3 on an AMD integrated GPU 27.5 -> 33 FPS against 720p.
- **Less GPU command thread work per draw:** the resource-table checks before every draw asked
  the memory map for every table (6% of that thread); confirmed mapped areas are now remembered
  until memory is unmapped (the checks went from 12% to 6.5% of the thread in Uncharted 3).
- **Fewer crashes from half-written resource tables:** a draw whose tables held garbage while the
  game was still filling them made the shader compiler stop the game ("Thread ID buffer addressing
  is not supported outside of compute"), and one bad pointer turned that table read into zero for
  the rest of the session. Such a draw is now drawn once with the bad entries empty.
- **Uncharted 3: Sully's shirt flashing as big flat-coloured triangles fixed** (at 40-60 FPS; it
  never happened at 30). The cloth simulation shaders sum values in shared GPU memory the way the
  PS4's 64-lane waves allow: without waiting between steps inside one wave. NVIDIA runs each wave
  as two 32-lane halves, which raced, so the shirt exploded on every other frame whenever the game
  used 2 cloth sub-steps (40-60 FPS; at 30 FPS it uses 3). Both cloth solvers (the shirt uses two)
  now get the waits, including inside the loop that repeats until the cloth settles (a value read
  at the end of one pass was overwritten at the start of the next). On an AMD GPU it never happened.
  Only those two shaders recompile, the rest of the shader cache stays.
- **Uncharted 3: Nate's legs flickering and NPCs floating a little fixed.** Like Uncharted 2 in
  0.1.2: the results of the game's small GPU copies now come back with the frame's "done" fence.
- **Much faster at 4K and in crowds (all three games), measured on a Ryzen 9 9950X3D + RTX 5080,
  frame limit 120.** The heavy scenes are limited by the CPU thread that feeds the GPU, not by the
  GPU, so this helps every PC (render resolution makes no difference there):
  - Uncharted 3 re-copied every crowd character's mesh data (about 43 MB per frame) from game memory
    on every use, because each piece is small enough to be treated like per-frame constants. Data
    that stays the same is now kept on the GPU and only copied again if the game changes it.
  - Fence values the GPU writes for compute work landed on memory pages that also hold GPU data, and
    each such write waited for the GPU (about 1.7 ms, ~40 times a second): now written directly.
  - Fewer barrier checks (overlapping views of one buffer counted once), lock-free checks before
    each draw, fewer thread wakeups for GPU interrupts, faster page fetches on a second GPU queue.
  - Results at 4K: Uncharted 3's Cartagena market 64 -> 79 FPS at the checkpoint and 69 -> 90 in
    the crowd (the items above; the second-queue fetches before them took a crowded street 46 -> 58),
    Uncharted 2's heaviest view 38 -> 99 FPS, Drake's Fortune 95 -> 101 FPS.
- **Multithreaded rendering is now much faster** (launcher, per game):
  compute fences no longer stop the recording thread. 4K: Uncharted 3 100 FPS, Uncharted 2 118,
  Drake's Fortune 117 (the 120 limit). On 4 CPU cores Uncharted 3 at 1080p runs at the 120 limit
  (90 without). 5 minutes of Uncharted 3 with it: no errors.

### Known issues (0.2.0)

- **Drake's Fortune:** the grenade launcher can kill the player (GitHub #6). Being looked at.
- Uncharted 3 on an AMD integrated GPU stopped shortly after a level started in earlier builds (the
  game waited for an internal job): not seen any more in a 6-minute test with the rooftop freeze
  fix (probably the same cause). Please report it if it still happens.

## 16 October 2026 (0.1.2)

- **Uncharted 2: blurry background while aiming fixed.** The aiming depth of field reads its focus
  distance from a 1x1 texture the GPU renders; it was never copied back to the game (the distance
  stayed 0, so everything behind Nate was out of focus). Tiny GPU-rendered textures now come back
  with the frame's "done" fence (Uncharted 2 only; the other two games do not need it). Above 1080p
  the game also measured the depth at the wrong spot (the crosshair's 1080p HUD position over the
  larger render): that position is now scaled to the render size, so it is right at every
  resolution.
- **Uncharted 2: GPU hang ("Device lost") after a long session fixed.** The treasure pickup effect
  takes some buffer descriptors from table slots that hold other data; one that pointed into mapped
  memory with ~1e9 records hung the GPU. Such descriptors (unaligned, huge stride or over 64 MiB)
  are now ignored for its two shaders.
- **Multithreaded rendering:** the Uncharted 3 crash within minutes (flips overtook the frame's
  fences; the GPU-written range set was read and changed by two threads) and the Uncharted 2
  floating NPCs (fences did not wait for their readbacks on the recording thread) are fixed;
  Uncharted 3 ran 8 minutes crash-free. Still experimental.
- **Faster heavy scenes:** optimizations that were off since the grey-world and floating hunts
  (threaded recording, draw preparation, prepared resources, deferred stream copies, parallel
  copies) are on again by default: heaviest Uncharted 2 view 43.8 -> 49.0 FPS, Uncharted 3 about 9%
  less GPU-command-thread work.
- **74-85% less GPU readback in all three games:** windows the CPU stopped writing into cool down
  after 3 s, and small GPU writes come back on their own instead of with the whole 64 KiB around
  them (the effect shaders' data the game never reads made up nearly all of it). Uncharted 2
  2.9 -> 0.46 GB per 5 s, Uncharted 3 3.1 -> 0.48, Drake's Fortune 0.63 -> 0.16.
- **Frame rate limits 40, 45, 50 and 55** in the in-game menu and the launcher.
- Profiler (`GOW3_PROFILE`): no longer freezes or crashes the game (own lock-free stack unwinder).
- New diagnostics: `GOW3_READBACK_STATS=1` (readback bytes per window, why and who wrote them),
  `GOW3_TRACE_PS=<hashes>` (render targets, viewport, textures and constants of a shader's draws).

- **Uncharted 3: crash in the Cartagena museum and characters flying around fixed.** GPU results
  were copied back to the game without protection: a late copy could land on data the game had
  just written (garbage pointers, a broken GPU command list). New GPU data is now read-protected
  until it is back; the game's reads of it wait, everything else runs on.
- **Uncharted 2: NPCs coming from the sky fixed.** The game places characters on the ground with
  GPU ray casts and reads the results once a GPU "done" counter is set. That counter was written
  when the command was recorded, before the GPU had run it, and a shortcut from 15 October threw
  nearly every result away. The counter is now written once the results are back in game memory,
  in strict order with the frame's other counters (as on the console).
- **Uncharted 2: NPCs floating in the air, Nate's feet sinking into the floor and NPC legs
  flickering while they walk fixed** (also in 0.1.1). The game reads back data that a GPU copy
  shader writes every frame; the port returned it with the next submission, a frame late. Those
  small copies now come back before the frame's "done" fence the game waits for (no FPS cost; the
  shader's large copies, which the game never reads, are left out). Every preset queues 1 frame
  again; the launcher moves preset settings back from 2 to 1.
- **Faster heavy scenes:** guest-memory copies for the GPU no longer take a lock (10-15% of the GPU
  command thread in heavy Uncharted 2 scenes: 7.9 -> 6.4 microseconds per draw); GPU checkpoints
  (crash diagnosis, ~2%) only on request (`UNCHARTED_GPU_CHECKPOINTS=1`).
- Protected copy-back without the cost: pages where the game reads its own data beside GPU results
  and results the game reads every frame (such as scene brightness) stay readable; Uncharted 2
  went from 33-46 to 60 FPS with protection on. A deadlock that hung Uncharted 3 while loading
  with protection on is fixed (uploads copy through an unprotected view of the same memory).
- Tracking of small GPU writes no longer grows over a session.


- **First release, 0.1.0 (beta).** The package now carries `LICENSE.txt`, `CHANGELOG.md` and
  the licenses of all bundled libraries (`licenses/`).
- **Faster heavy scenes with every preset: 2 frames queued** (Low and Medium had 1). With 1 the
  CPU waited for the GPU to finish the previous frame every frame; in Uncharted 2's museum
  (1080p) that was 44-47 FPS, with 2 it is 56-59. The launcher moves existing settings over
  (not for a game with multithreaded rendering, which Uncharted 2 needs with 1).
- The game and launcher windows name the port's author in their title bar.
- **Launcher updates:** at start the launcher looks for a newer release and offers
  **UPDATE TO x.y.z**; it downloads the package, checks its SHA-256 against the one GitHub publishes,
  installs it with the new version's own installer (`tools/apply_update.py`: saves, settings and the
  shader cache stay; on any error every replaced file is put back) and restarts. Advanced tab: the
  check can be turned off or run now. Tested against a local fake release (install, restart,
  cleanup) and a locked file (full rollback of 1,077 files).
- **Automatic releases:** `tools/release.sh X.Y.Z` pushes the tag `vX.Y.Z`; GitHub Actions builds
  the package and publishes the release (marked Latest) with the notes from `tools/release-notes/vX.Y.Z.md`.
- Profiler: call stacks (`profile_*_stacks.txt`), also of the game's own thread that submits the
  frames, so a wait shows who waits.
- Packaging: the license step no longer hangs on GitHub Actions (the MSYS2 root `/` made
  `//usr/bin/pacman` a network path).
- **Uncharted 2: Nate floating above the ground or invisible fixed.** A CPU shortcut for one of
  the game's small GPU copy shaders (6a6422a9) copied the player's data before the game had
  finished it; that copy runs on the GPU again.
- **Uncharted 2: crash with multithreaded rendering fixed** (game assert `m_gfxEopTick` in
  `FrameBegin` after 1-3 minutes): GPU memory copies and fills no longer overtake the frame's
  end-of-pipe timestamp. Still open with 2 frames queued, see the known issues.
- **Heavy scenes: stalls removed.** CPU writes into ring memory whose old GPU data was still being
  read back waited for the GPU (a third of each frame with multithreaded rendering); the
  per-draw stream buffer grew from 128 to 512 MiB (it filled up with 2 frames queued);
  "Frames queued 2" is now the High preset's default. Heavy Uncharted 2 scenes: about 33 -> 40-45
  FPS (4K, multithreaded off).
- Built-in sampling profiler for the GPU threads (`GOW3_PROFILE=<seconds>`).
- **Drake's Fortune: bullet impacts, bullet holes and blood.** The game traces its bullet and
  decal rays on the GPU (`Cs_RayTrace`, `Cs_RayTraceGatherHits`) and reads the hits once the
  frame's end-of-pipe event fires. That event was signalled before the GPU had produced the
  hits, so every shot read as a miss. The results are now copied back asynchronously with their
  pages read-protected until they arrive.
- **Drake's Fortune: cutscene movie colours at 1620p** (grey picture with diagonal coloured
  lines). The GPU reads the movie's colour plane with rows padded to 64 texels (1472 at 1620p);
  the scaled frame copy now writes its rows the same way.
- **Frame-rate limit capped at 120.** At 140-160 FPS Drake's Fortune's movement broke (Nate fell
  through the world). The launcher and the in-game menu offer 30 / 60 / 120; older, faster
  settings are read as 120.
- **Launcher and in-game menu settings stay in step.** Closing the in-game menu no longer
  overwrites a change made in the launcher while the game runs, and the launcher shows what the
  in-game menu saved.
- **Uncharted 2: crashes at treasure pickups and in the Treasures screen fixed.** CPU-rewritten
  per-frame data (skinning tables) was corrupted by a byte-compare merge of GPU data; the merge
  is gone, and resource descriptors that cannot be real are ignored instead of crashing the GPU.
- **Uncharted 2: frame rate no longer falls over time.** In a busy scene it slid from about 42 to
  31 FPS within minutes: a per-page list of GPU writes grew every frame and was scanned on every
  buffer binding. Found with a new built-in sampling profiler (`GOW3_PROFILE`); the frame rate now
  stays level.
- **Frame drops in heavy scenes reduced.** CPU writes next to GPU data in flight no longer wait
  for the GPU; scalar copy shaders run on the CPU; GPU readbacks are asynchronous.
- **Uncharted 2: torch follows Nate** (launcher option, Advanced tab). *In test.*
- **Update 1.02** used automatically when it sits next to the game folder.
- **Render resolution above 1080p:** Uncharted 2 and 3 up to 4K, Drake's Fortune up to 1620p with
  FSR 1 (its memory layout); cutscene movies scaled to the render size.
- Keyboard and mouse: mouse look and remappable controls per game; display modes fixed.

## 14 October 2026

- **Grey/black ground and unlit Nate fixed:** constants the GPU writes are synced back to the CPU.
- **Sound effects** (gunfire, footsteps, ambience) fixed.
- **Dark interiors:** fire and flashlight light fixed; bright hair and cobwebs fixed (MUBUF ADDR64
  loads).
- Uncharted 2 and 3 start from the Collection menu; the Uncharted 2 new-game freeze fixed.
- The Uncharted launcher: per-game settings, quality presets, render resolution.

## 13 October 2026

- Gameplay reached: the Collection's game code runs natively on Windows.
