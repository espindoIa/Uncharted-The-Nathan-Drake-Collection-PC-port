# Uncharted compatibility work — 13 October 2026

## Index (grep the heading text)

New entries go at the end, dated, measured numbers only.

- Grey/black ground: `Grey/black ground investigation, 14 October 2026 (afternoon)`, `FIXED 14 October 2026 (18:50): grey/black ground, unlit Nate`
- Sound effects: `Sound effects silent (gunfire, footsteps, ambience); music and dialogue fine (14 October 2026, evening)`, `FIXED 14 October 2026 (19:3x): sound effects (footsteps, ambience, gunfire)`
- Readbacks and ray-cast fences: `16 October 2026: readbacks, ray-cast fences, floating NPCs (0.1.2)`
- Floating NPCs: `16 October 2026 (night): U2 floating NPCs / sinking Nate / leg flicker - real cause and fix`
- Upscalers, DLSS, motion vectors: `18 October 2026: object motion vectors in the Uncharted games; DLSS from NVIDIA`
- DLSS model M cost: `20 October 2026: model M's own cost (same model, same input and output)`
- Low-end PCs: `17 October 2026 (night, 02:30-04:40): low-end PCs`
- Radeon 780M: `18 October 2026 (night, laptop): Radeon 780M optimization (RTX 5060 switched off)`
- U3 performance at the player's settings (also lists the measuring tools and A/A noise): `19 October 2026: Uncharted 3 performance at the player's settings`
- Repo audits: `18 October 2026: GPU-offload audit, register scan, constant ring reuse`, `20 October 2026: repo audit fixes`, `20 October 2026: audit leftovers, second audit, performance quick wins`
- Fork (espindoIa) notes: `Fork (espindoIa) notes, merged onto 0.3.0 (19 Oct)`
- Multi-core roadmap phases (22 Oct): `22 October 2026: multi-core roadmap phases 0-2 and 5`, `22 October 2026: stage B dispatch fast paths (roadmap phase 4)`
- Reflex and Frame Generation (decisions, measurements): `Reflex and DLSS Frame Generation (22 Oct)`
- Reflex latency: `Reflex, corrected measurement (22 Oct, second round)`
- FG frame limit and pacing: `Frame limit and pacing with Frame Generation (22 Oct, second round)`
- Reflex + FG together: `Reflex + Frame Generation, one short run (22 Oct 2026)`
- Reflex GPU idle diagnosis and early submit: `Reflex: GPU idle diagnosis and early submit (22 Oct 2026)`
- FG model selector (none in NGX 310.9.16): `Frame Generation model (NGX)`
- RTSS / MSI Afterburner OSD flicker, present queue, dev NGX banner: `RivaTuner / MSI Afterburner OSD flicker: the present queue (22 Oct 2026)`
- Streamline backend (DLSS, FG, Reflex through SL 2.14.16; FG needs the window focused; the only option since the same day, NGX as fallback): `NVIDIA Streamline backend (23 Oct 2026)`, `Streamline as the only option (23 Oct 2026, later)`
- Measuring method (ab2.sh, abstats.py, PresentMon, profilers): see the Tools list in the U3 performance section above, and AGENTS.md "Measuring".
- U3 market stutter and CPU work (earlier 23 Oct benchmarks stood at the save's gate): `Cartagena market: stutter and lower frame rate from readback faults (23 Oct 2026)`.
- Structural crowd experiments and exact rollback: `Structural crowd snapshots and output grouping (23 Oct 2026)`.
- Market frame rate = Reflex sleep, pacer replaces it: `Reflex pacer: market frame rate (24 Oct 2026)`
- Upstream merges: `21 October 2026 (night): upstream 0.5.0 merged`, `Upstream 0.6.0 merged (24 Oct 2026)`
- Launcher overhaul (sidebar, Home checklist, keyboard/controller, dialogs): `24 October 2026: launcher overhaul`
- Session recorder and engine flight recorder (retroactive per-flip CSV for one-frame flashes): `25 October 2026: session recorder`, `25 October 2026: engine flight recorder`
- Upscaler history reset on camera cuts, flight `d_compiles` fix: `25 October 2026: history reset on camera cuts`
- Scene LOD bias skips explicit-LOD samplers, forced anisotropy skips unmipped ones: `25 October 2026: scene LOD bias and anisotropy follow the shader's sampling`
- Lamp shadow passes found by what they sample and jittered: `25 October 2026: lamp shadow passes get the scene jitter`

- Game FXAA skipped under temporal AA: `Game FXAA skipped under temporal AA (24 Oct 2026)`

- Text and subtitles at 4K (font atlases, text shader sharpening): `Sharp text above 1080p (25 Oct 2026)`

- Launcher redesign (stage, per-game colour, 4K icons, trophy coins): `Launcher redesign and 4K-sharp drawing (25 Oct 2026)`

Target: CUSA02344 v01.00, eboot.bin (Drake’s Fortune).

The game boots through the Collection menu and opening movie into gameplay.
The user confirms normal controls and working audio after the clock correction.
3D appeared briefly, but movement led to GPU device loss. Work is incomplete.

Current investigation: dynamic shader resource reads. The GOW3 default disables
GPU direct memory access and substitutes flat-buffer data for unflattened reads.
The latest run enables GOW3_DIRECT_MEMORY_ACCESS=1; outcome is not yet verified.

Implemented: package/metadata extraction; three-executable preparation; full
fiber context switching; FFmpeg Videodec2; memory range names; dynamic GPU hook
queue; installed PlayGo metadata; disk space; offline services; concurrent event
queue lifetime safety; interrupt registry; optional executable placement below
2 GiB; buffered controller reports; optional native CPU clock consistency.

Clock tests passed in both modes. This CPU runs at about 4.3 GHz, whereas the
base runtime reports 1 GHz. Uncharted executes inline RDTSC instructions; the
launcher now measures and reports the matching hardware frequency.

Shader changes: support uniform ReadFirstLane resource offsets, balanced walker
stack on failed pointer offsets, bounded GPU semaphore diagnostics with explicit
error handling. Dynamic Phi/lane-dependent resource offsets remain unsupported
by the CPU walker; the GPU memory-read path is being tested.

Settings: vsync and 60 FPS; background shader compilation, draw pipeline, Vulkan
recording threads, draw preparation, prepared resources, deferred stream copies
and parallel copies disabled during diagnosis.

Launch: ../Start-Uncharted.cmd → tools/probe_uncharted.py →
gow3_pc/out/gow3-probe.exe. Game data: game/CUSA02344/uroot. No timeout.

Native tests passed for fibers, memory names/notification ordering, services,
content, concurrent handles, video decode, buffered pad and clock timing.
Windows build passes. Uncharted 2/3 gameplay has not been tested.

Logs, attempts and frame captures: uncharted/. Thread snapshot: thread-sample.json.
Older import counts predate later API implementations. REA opened the reconstructed
ELF, but decompilation was unavailable with the installed Windows providers.

## Update 13 October 2026 (evening): gameplay reached

The device loss on the way into gameplay (menu blur / loading, cutscene skip) is fixed.
Cause: TextureCache::FindDepthTarget linked any image starting at the depth buffer's
stencil address as its stencil proxy. Uncharted reuses that memory for a 960x540 RGBA8
texture; sampling it was redirected to the 960x540 D32S8 depth image and the GPU read past
that image's memory (VK_EXT_device_fault: ReadInvalid page fault). Only 8-bit images or the
format-less placeholder are now linked, and GetAssociatedDepth drops stale links.
The user confirmed gameplay after the fix.

Also fixed: evicted samplers are destroyed only after the GPU passed them (deferred).

Diagnostics (GOW3_GPU_CHECKPOINTS=1, set by tools/probe_uncharted.py; =2 adds
CommandBuffer() caller markers): on device loss the log shows the device fault report,
the draws/dispatches with their bound images and buffers after the last completed marker,
and which Vulkan object/guest image owns the fault address (device address binding report
plus image/buffer placement records). Diagnostic-only switches: GOW3_KEEP_FREED_IMAGES=1,
GOW3_FEEDBACK_LOOP_LAYOUT=0. The probe still disables several GOW3 performance features.

## Update 14 October 2026

- FrameBegin assertion (fg-draw-mgr.cpp:234, m_gfxEopTick of frame N-3): the game's wait for a
  frame's GPU end-of-pipe stamp (0x3fd6xx) gives up after 2.0 s. tools/probe_uncharted.py now
  NOPs that timeout (`ja` at 0x3fd816) in boot-linked.bin by unique pattern (EBOOT_PATCHES).
- Lighting: tiled lighting compute cs 0x43b8ee5e indexes 224-byte light records with a
  GPU-computed index (waterfall loop, v_readfirstlane); the SRT walker cannot pre-read them.
  GOW3_DIRECT_MEMORY_ACCESS=1 is now the probe default. Its per-light shadow texture
  descriptors (8 "not flatenned") remain unresolved.
- Jungle chapter (temple-0a/0b) once showed a grey world (level geometry, shadows and sky
  probe computes not rendered; Nate black) until a treasure pickup; all packs were read
  completely, PlayGo reports all 100 chunks available. After the new save it loads fine.
  Cause not established; watch for it on new chapter loads.
- Pipeline cache and background shader compilation are on again in the probe; GOW3 stale and
  deferred readbacks are off in analysis/uncharted-user/probe.ini.
- Diagnostics added: GOW3_FILE_TRACE=<text> (timestamped opens/reads), GOW3_CAPTURE_PASSES /
  number in the capture trigger file (frame capture of N passes), GOW3_SKIP_FILE (live list of
  shader hashes to skip), shader hashes in SRT/sharp warnings.
- Audio output timing is clean (no starvation); latency ~83 ms queued plus device buffer.
  Gunfire audio missing and black circles on bullet hits are still open.


## Diagnostic update 14 October 2026 (01:xx)

User-confirmed gunfire decrements ammunition but remains silent. Recorded input PCM on all eight main channels is zero during shots, before volume/downmix; port gains are nonzero, sound category gains are 1, and output never starves. Common/temple sound banks open and read successfully. Missing effects occur before SDL output conversion.

Shader compilation disabled and low renderer cache optimizations disabled did not recover the grey scene (mask restored to 159760). Native process counter now shares TSC units/frequency; clock tests pass, but user confirms this did not fix either symptom.

Compared Supermedo/bloodborne_pc windows commit e5d04e2 (see bloodborne-comparison.md). Ported cached SRT walker handler registration and speculative/concurrent fault safety, preserving local locking and balanced failed PushPtr. Build passes; scene remained grey. Shader binary/meta cache versions bumped 7 to 8 so earlier compiler output cannot silently survive updates. Latest test disables both pipeline and binary cache for fresh compilation.

One-shot guest image diagnostics added to Presenter: guest-images.trigger in GOW3_DUMP_DIR lists hex address and guest size; copies depth/stencil or uncompressed color. Only activates on frame-dump trigger. Intermediate capture shows depth containing props/characters, normals containing additional geometry, two lighting targets RGB zero. Capture files in uncharted/captures/1791923083373662700. New capture notes include precise per-draw depth/stencil flags. Root cause still open.

Latest follow-up: fresh shader test (pipeline and binary cache both disabled) still grey. Depth/stencil flags and stencil plane captured under uncharted/captures/1791923397807259500, pass notes in uncharted-user/captures/frame_20261014-013133_1.txt. Stencil has values 0/128/192/194/208/210, not entirely zero. Lighting output addresses rotate; cs43b8ee5e writes 0x11438f0000 in this capture, so zero 0x11408b0000 alone does not prove that compute failed. Presenter captures may observe a subsequent in-flight frame's depth; need capture at the specific rendering stage before attributing missing geometry to depth.

Corrected CLOCK_PROCTIME (ID15) to elapsed time since process start, rather than accumulated host CPU time. Regression tests pass default/native. Trace currently observes clock ID4 only; no evidence that ID15 caused current symptoms. Latest run session15495, capture folder1791923766600283800, still grey. Exe gow3_pc/out/gow3-probe.exe, launcher Start-Uncharted.cmd. No goal completion claimed.

## Update 14 October 2026 (later): fixes and the open "grey world" investigation

Fixed (in the build):
- Event queues (gpu/shadps4/core/libraries/kernel/equeue.cpp): the poll path (timeout 0)
  read and cleared triggered events without the queue lock, losing a concurrent trigger;
  waits with a pending small HR timer ignored other triggered events; TriggerEvent woke one
  waiter only (now notify_all).
- Shader cache key vs DMA: shaders compiled with GOW3_DIRECT_MEMORY_ACCESS=0 stay in the
  pipeline cache; the old cache was moved to analysis/uncharted-user/cache-before-1014.
- Evicted samplers are destroyed after the GPU passed them (texture_cache.cpp).
- Audio latency: queue target 1-2 periods (GOW3_AUDIO_QUEUE_BUFFERS, default 1) and a 480-frame
  WASAPI buffer request (GOW3_AUDIO_DEVICE_FRAMES): ~41 ms queued instead of ~83 ms, no starvation.

Diagnostics added:
- GOW3_FILE_TRACE=<text>: timestamped opens/reads/seeks; every failed open is now logged
  (errno, file table full, path translation).
- GOW3_AUDIO_TRACE=1: Ajm instance create/destroy always, RIFF jobs, 5 s decoder totals;
  sound1/ paths traced.
- GOW3_RETEXTURE_FILE=<file>: creating the file marks all CPU-sourced textures for re-upload.
- GOW3_SKIP_FILE, GOW3_CAPTURE_PASSES / pass count in the capture trigger file.
- Scratchpad tools (memory peek/watch/snapshot) read game globals at guest base 0xb400000.

Grey/black world after loading the jungle (intermittent, reproduced via main menu -> Continue):
- Workaround: Pause -> Treasures -> back (a plain pause or other menus do not help).
- The game renders reduced material variants: no sun shadow pass, no level G-buffer pass,
  no sky/ambient computes (cs 2a2e99be, 0a57ac2f, 43225bd9); ground flat or missing, Nate
  sometimes unlit.
- Texture streaming (texturedict1/*-tstrm.pak) reads stop right after the load and resume in
  a burst when Treasures opens. The streamer object (0xe66800) stops changing; its update gate
  flag (0x994188+0x39b7) stays 1.
- Ruled out: failed file I/O (all reads succeed), PlayGo (all 100 chunks available,
  0xd30df0), package loading (all 74 packages status 12, manager 0xd1b988), stale GPU
  textures (forced re-upload of 447 textures: no change), readback mode (Precise: no change),
  occlusion queries (always visible), Windows red-zone clobbering (measured intact on the
  Ryzen 9 9950X3D), SRT/DMA lighting.
- Correlated: the menu host (0xabb148) page stack top index (+0x790, pages at +0x590) is 2 in
  the broken state and 0 after the Treasures fix. Forcing it to 0 did not fix the world, so it
  is a symptom of the same condition.

Audio: only music plays in the broken state (level-driven sounds absent). Sound banks
(common.bnk etc., mono ATRAC9 RIFF) are opened; no bank voice reaches Ajm. Gunfire was the
only missing sound when the level loaded correctly.


## Grey/black ground investigation, 14 October 2026 (afternoon)

Ruled out (bad Continue load compared with the same load after Pause -> Treasures -> back):
- Texture streamer gates: manager 0xd60590 (dispatcher 0x3dfb90 registered at 0xe66710,
  mode 2 path 0x3e1b90). Enable byte +0xf1530 = 1, pending-op ring +0xf1558/+0xf1560 empty,
  streaming-state arg null. Needed-set +0x8c0 empty and request slots (0xe66800, stride 0x88,
  state +0x50) idle in good and bad loads alike, so the stall is a symptom (nothing is drawn,
  so nothing is requested).
- Rendered-background list (UpdateBackgroundsForRendering 0x3aeb10, manager 0xfdb5f0: list at
  +0x0, count +0x180): the same 4 level backgrounds in good and bad loads. Their objects and
  all objects they point to are unchanged by the Treasures fix (only a camera-following
  transform differs; +0x80 is a "transform changed" flag, set at 0x3afdcb).
- Debug flag "Draw Levels Outside Vis" (0x98f8e0+0xc5) forced on: ground still black.
- FgDrawMgr (0xd5c500) slot bitsets: no difference. Thread stacks: no stuck thread.
- The Treasures screen registers a treasure model with FgDrawMgr (0x3c65a0) and loads two
  packages (package manager 0xd1b988 +0x100/+0x168 go 7 -> 9).

In the bad load, grass, rocks and characters draw but the ground under them is black, and
the fix is only partial (large black ground areas remain). Next step: the frame capture now
records indirect draws and indirect dispatches, with arguments as the CPU sees them
(vk_rasterizer.cpp DrawIndirectRecord/DispatchIndirectRecord); earlier captures omitted
them, so "no level G-buffer pass" may have been indirect draws that were not recorded.
Launch with GOW3_CAPTURE_TRIGGER=<file> (write a pass count into the file) or
GOW3_FRAME_CAPTURE_KEY=1 (F11).
Tools: scratchpad heapsnap.py (full guest memory save/diff in ~5 s), threads.py (guest stack
sampler), bgdump.py.

### Capture comparison, same load, bad vs after Treasures fix (10:40 / 10:42)
Captures: uncharted-user/captures/frame_20261014-104041_1.txt (bad) and
frame_20261014-104215_2.txt (fixed), 600 passes each (GOW3_CAPTURE_TRIGGER with a pass count).
- No indirect draws or dispatches at all (new capture notes would show them).
- Both frames issue the same passes and draws (7602 vs 7612 draws, 78 computes, same shaders).
  The ground IS drawn in the bad state; it renders black. Earlier "no level G-buffer pass"
  conclusion is wrong for this repro.
- Frame structure: depth prepass interleaved with one-draw depth-only test passes (ps d3cd03d8 /
  bb6ab1e5, sampling an R8 64x64) = occlusion-query boxes; ZPASS_DONE results are synthesized
  as "visible" (liverpool.cpp EventWrite), SET_PREDICATION is ignored.
- Only sampled-texture difference: 4 streamed textures. Bad: 0x11547a0000 BC5 512, 0x1155038000
  BC1 sRGB 512, 0x1155064000 BC1 512, 0x115853c000 BC1 64. Fixed: 1024x1024 versions at new
  addresses (0x115ef84000, 0x115f938000, 0x11603bc000, 0x1164578000, 0x116478c000).
- Working hypothesis: the low-mip ground textures hold stale/black content in the texture cache
  (filled by a path the cache does not observe, e.g. GPU/DMA copy, or marked GpuModified so the
  forced re-upload skipped them); the Treasures fix streams in higher mips at new addresses,
  which are created fresh. Next: dump guest bytes at those addresses vs cached image contents.


### Review and texture evidence, 14 October 2026 (10:50)

Reviewed the 10:40/10:42 comparison. Important limitation: vk_frame_capture.cpp
AddSampled keeps only the first 32 unique sampled/storage textures per pass;
AddShader keeps only 8 shader hashes. Thus "only sampled-texture difference: 4"
is a bounded observation, not a complete comparison of all materials/shaders.

Added one-shot BC1/BC5 cached-image dumps (base mip, layer 0) to the existing
Presenter guest-images.trigger diagnostic, plus guest layout/flags metadata.
It does NOT call UpdateImage, avoiding an accidental refresh before capture.
Build passed. Latest running capture folder: uncharted/captures/1791956845059173200,
pass capture uncharted-user/captures/frame_20261014-104946_2.txt. This save resumes
near a rock arch and still has missing/grey terrain and black Nate.

Actual decoded GPU images for the corresponding four current-run addresses:
- 0x115478c000 BC5 512: pistol normal map.
- 0x1155024000 BC1 sRGB 512: pistol colour/material atlas.
- 0x1155050000 BC1 512: pistol material mask/atlas.
- 0x115852c000 BC1 64: small pistol-associated mask (sampled alongside the above).
The former 10:40 addresses differ by allocation placement in this run. These
captures do not support attributing those four texture changes to the ground.
See texture-comparison.png (GPU left, CPU micro-detile interpretation right).

Read-only guest snapshots: tools/snapshot_guest_textures.py <capture-directory>.
Comparing base-mip compressed-block multisets removes ordering/tiling as a source
of false mismatches: the four suspect textures differ from current CPU bytes;
other captured textures match exactly (0x11549f4000, 0x1154a00000, 0x1154ecc000).
All nine captured images have flags 0x40 (Registered, no Dirty/GpuModified/Aliased).
GPU pistol images are recognizable and nonblack; CPU bytes may have been reused
or overwritten. A mismatch is observed, but does not yet establish which copy is
correct or explain the missing ground. Do NOT force arbitrary CPU re-uploads as
a fix based on this comparison alone.

Earlier opt-in GOW3_DEPTH_PROBE_FILE diagnostic remains available (0 guest,
1 Equal->LessOrEqual, 2 Equal->Always, only draws without depth writes). It has
NOT been tested in gameplay and is not enabled in this run. No graphics or audio
fix is claimed from this review.

### Main-pass buffer comparison, 14 October 2026 (11:07, reviewed later)
analysis/bad-mainpass.txt vs good-mainpass.txt (pass #154, 795 draws, same shaders):
- Of 17 recorded buffers, exactly one differs zero/non-zero: the first draw (vs da3d9c98 /
  ps 45d0299c) slot 1, 42616 bytes, 28-byte vertex stride (positions about -8..9). Bad: all
  zeros (0x11009cd520). Good: filled (0x1100ecd520).
- The same draw's slot 0 constants [158..170] are zero in bad, and in good hold
  1,1,1, 107/255,88/255,73/255, 5,25,4,60, 0.2,0,1 (looks like a colour plus parameters).
- The vertex data is not stored verbatim in temple-0*/common packs, so it is generated per frame.
- tools/scan_ground_state.py (read-only) searches live memory for both patterns -> analysis/ground-scan.log.

### Ported from the MediEvil port (reference/medievil, fork base ce1886a), 14 October 2026 17:28
- (reverted after the 17:47 Fios EIO crash) runtime_mutex.c retirement of destroyed mutexes/conds;
  a destroy of a held mutex returns EBUSY.
- Depth bias constant scaled by PA_SU_POLY_OFFSET_DB_FMT_CNTL (regs.h 0x2DE) vs the host depth
  format; the log prints "GPU: depth bias ... scaled" once if it ever applies.
Build passes. No mutex unit test exists. Not relevant to the grey ground: MediEvil's CMask fill
(Uncharted never logs "Unhandled metadata") and memory-pool APIs (not imported by Uncharted).

## Grey world: cause found, fix pending test (14 October 2026, evening)
Live bad state (after main menu -> Continue): menu host 0xabb148 page stack top (+0x790) = 2,
stack = slot +0x3b0, slot +0x3c0, slot +0x448. +0x448 (and its twin +0x440) is the one-button
message page; descriptor host+0x8f0 = message 0x155d235e, button 0xc6467f2d, action 0x2dac.
Code (llvm-objdump of analysis/uncharted/eboot.elf; REA cannot open this ELF on Windows):
- Menu step 0x14a8b0 (also 0x149698, 0x14a986): if 0x577b60(&0x11a69d0) is false, toggle
  0xabbaf4 and push message 0x155d235e on page +0x448 or +0x440 (alternating). OK (0x2dac)
  goes to menu state 6, the same as the "ready" path (0x14aa2c).
- 0x577b60: true if flag 0x994188+0x3a04 == 0 (a general flag, read by the renderer too),
  otherwise object state (+8) not 0 and not 2. Object 0x11a69d0 is the PSN TUS client
  (its state machine near 0x578000 calls sceNpTus*/np_signed_out through GOT 0x9875d0..0x987608).
  Signed out, state stays 0.
- The message page stays on the stack in gameplay, so menu overrides persist: level sounds muted,
  level lighting constants zero (ps 45d0299c [158..170]), ground vertex buffer empty.
  Pause -> Treasures -> back clears the stack.
Fix: tools/probe_uncharted.py EBOOT_PATCHES makes 0x577b60 return true (b0 01 c3). Other callers
(0x149083, 0x14ca5c, 0x14d160) also require +0x28c4 ("TUS data loaded"), which stays 0.
Not yet tested in game. Test: Continue at least 3 times from the main menu; check the ground,
level sounds, and that the page stack top (image 0xabb8d8) is 0.
- 17:47 run with the PSN patch plus the MediEvil lock port crashed during the temple-0a -> 0b load:
  ASSERTION ErrFromFios (ndlib/io/streaming.cpp:523), SCE_KERNEL_ERROR_EIO, fault 0x395ac2.
  First occurrence. The lock port is reverted: its cond_destroy always destroyed even with waiters,
  while the PS4 returns EBUSY there, so a Fios wakeup could be lost. Depth-bias port and PSN
  patch kept. Rebuilt 17:5x.
- PSN patch tested 17:5x: popup gone, world still grey. Menu stack then [+0x3b0, +0x3c0], top 1.
  The menu host mode (+0x0; 1 = front end, 2 = in-game; IsFrontEnd 0x14adf0 = mode < 2) is 1 on bad
  loads and 2 after Treasures (0x14b2e0 opens the in-game menu: SetMode 0x148630(2) and replaces the
  stack with slot +0x468). Writing mode 1 into a good state changed nothing visible, so it's a symptom.
  PSN patch reverted.
- Key evidence (same session, 11:06:15 bad vs 11:06:59 after Treasures): the ground draws (vs
  823cb43a pass #137, vs da3d9c98 pass #154) read a 42616-byte vertex buffer that is all zeros in
  guest memory at 0x11009cd520. After Treasures the same mesh is filled at 0x1100ecd520 (+5 MB),
  and the SRT constants [34..37] (25, 4, 60, 0.2) of ps af7700b5 are filled too. So level geometry/data is
  allocated but never filled on bad loads; Treasures reloads it. The fixed state still shows grey holes
  in the distance (screenshot via PrintWindow).
  Suspect: level data streaming via Fios (ndlib/io/streaming.cpp; the same code asserted EIO at 17:47).
- Next run: GOW3_FILE_TRACE=.pak (probe default for now, 200000-line cap) to see whether pak/stream
  reads stop or fail on a bad load. tools/globals_diff.py (image globals save/diff) and
  tools/peek_any.py added.

### 14 October 2026 (evening): ground geometry is not generated on bad loads
- Reference review: reference/bloodborne (Supermedo windows branch) and reference/shadps4 (sparse,
  kernel/fiber) show no file, lock or DMA fix that applies. Our DMA CopyBuffer matches upstream, and
  FindImageFromRange already defaults to ensure_valid.
- .pak trace (GOW3_FILE_TRACE): every read succeeds; temple-0a/0b are read completely; streaming reads
  stop after the load.
- Per-frame GPU ring at 0x1100000000 (1 MB per frame slot; tools/region_map.py): bad frames write about
  0x90000 bytes, good frames 0xC0000. The extra data is ground vertices (stride 28, constant 0.00785
  before each position), so the game's CPU code doesn't emit the ground on bad loads.
- Menu host forced to the good configuration (mode 2, stack [+0x3b8], top 0) in a bad state:
  still black. Menu state is a symptom.
- Globals diff, two sessions (tools/globals_diff.py, analysis/globals/*): 60 consistent differences,
  e.g. allocation bitmaps of the object at 0xe924f0 (vtable 0x982810, methods 0x73xxxx/0x74xxxx,
  block 0xe92a10 size 0x5860) all free on bad loads; 0xe65190/0xe652e0 0 vs -2.
- Next: find the ground vertex writer with a hardware write watchpoint in a good state.
  The runtime now logs "Watch:" lines (probe.c watch_hit); arm with tools/watch_write.py <addr...>,
  e.g. ring offsets +0xa0000 of the active slots. Profiles: tools/sample_guest.py + profile_report.py.
- Watchpoints (DR0-3 on all threads, slot +0xa0000 of the active 1 MB blocks at 0x1100x00000): no CPU
  write hits, before or during a second Treasures. WriteProcessMemory to these pages fails (GPU page
  tracking keeps them write-protected), so an earlier "value restored" test was invalid.
- Leading model: the ground vertices are produced on the GPU. cs 308607aa (18 dispatches per frame,
  groups 26x1x1) reads compressed meshes from package memory (0x119e...) and writes decompressed
  vertices into 0x1100...; captures listed only its first 4 dispatches. The ground draws (vs 823cb43a,
  vs da3d9c98) read 0x1100xcd520.
- Capture now lists every draw's and dispatch's buffer addresses with GOW3_CAPTURE_ALL_BUFFERS=1
  (probe default), triggered by writing a pass count into analysis/uncharted-user/capture.trigger.
  Next: bad vs good capture; check whether a 308607aa dispatch writes the range the ground draws read.
- 18:3x: the ground shader's lighting records (64-byte table at 0x1038ee8380.., referenced through
  V# 0x1038ee858d in ps 45d0299c's flattened SRT data; same address in every run) are zero in bad
  states. After Treasures, 0x1038ee8500 became 1,1,1, 0.42.. with no CPU write (DR watch armed), so
  the GPU writes this table. Our SRT walker reads it from guest memory on the CPU at draw recording:
  candidate CPU/GPU ordering or coherency bug.
  Also, GPU compute output (e.g. 7d7e7206, 308607aa) is not visible in guest memory, so zero vertex
  buffers in guest memory prove nothing.
- Added GOW3_WATCH_RANGE=lo:hi (gpu/shim/gow3_watch_range.h; probe default 1038e00000:1038f00000):
  logs storage-buffer binds, DMA copies (CPU/GPU path), fills and WRITE_DATA into the range.
  Build 18:3x installed; the running game used gow3-probe.1818.exe (renamed).
- 18:4x ROOT CAUSE CANDIDATE: GOW3_WATCH_RANGE shows shader 2d215c3b (storage slot 1, many 16-172-byte
  writes) writing the constant memory 0x1038e7.../0x1038eb... that the ground shaders' SRT data points
  into. GOW3_READBACKS defaults to Relaxed (emulator_settings.h): GPU-modified pages are not
  read-protected, and GPU data reaches guest memory only when the CPU writes that page (write-fault
  readback). The CPU-side SRT walker therefore flattens stale zeros: ground constants (transform and
  lighting) are zero, so the ground collapses and Nate is unlit. Treasures happens to cause a CPU write to
  the page, which triggers the readback.
  Test: GOW3_READBACKS=2 (Precise) as probe default. The earlier "Precise: no change" predates this
  evidence.
- 18:4x CONFIRMED: in a grey state (no menu use), copying the GPU-modified range 0x1038e00000-0x1038f00000
  back to guest memory (GOW3_READBACK_TRIGGER "1038e00000 1038f00000"; BufferCache::DownloadRanges)
  restored the ground and Nate's lighting at once. Cause: GPU-written constants (copy shader 2d215c3b),
  flattened by the CPU-side SRT walker from stale guest memory under Relaxed readbacks.
  Precise readbacks (GOW3_READBACKS=2) hang on Continue. Fix candidate: GOW3_SYNC_GPU_CONSTANTS
  (Rasterizer::SyncSmallGpuWrites copies storage writes of 4 KB or less back at each submission,
  merged into 64 KiB windows).

## FIXED 14 October 2026 (18:50): grey/black ground, unlit Nate
Cause: the game writes per-object constants (ground transform and lighting records in 0x1038e.....)
with a GPU copy shader (cs 2d215c3b). Draws read them through SRT pointers, which our CPU-side SRT
walker flattens from guest memory. With GOW3_READBACKS=Relaxed (default) GPU writes reach guest
memory only when the CPU later writes the same page, so the walker flattened zeros. Pause -> Treasures
-> back fixed it only because a CPU write landed on that page and caused the write-fault readback.
Fix: Rasterizer::SyncSmallGpuWrites (vk_rasterizer.cpp) plus BufferCache::DownloadRanges. Shader
storage writes of 4 KB or less are copied back to guest memory at each submission (64 KiB windows,
one GPU wait). GOW3_SYNC_GPU_CONSTANTS=1 is the probe default; it costs about 1 FPS (57-59 FPS).
GOW3_READBACK_TRIGGER file: "auto"/"off", or "<lo> <hi>" for a one-off range readback.
Verified: a live grey state was fixed by a one-off readback of 0x1038e00000-0x1038f00000, and auto
mode is on. Fresh Continue loads with auto mode from launch are still to be confirmed by the user.
Not fixed by this (open): silent gunfire, black bullet-hit circles, the Fios EIO crash seen once
(17:47).
Ruled out on the way: PSN TUS popup and menu host mode (symptoms), Precise readbacks (hang on load).
Diagnostics left in the code (off by default): GOW3_WATCH_RANGE, GOW3_CAPTURE_ALL_BUFFERS,
Watch: debug-register hit logging in probe.c (tools/watch_write.py), tools/globals_diff.py,
sample_guest.py, region_map.py, peek_any.py.

## Sound effects silent (gunfire, footsteps, ambience); music and dialogue fine (14 October 2026, evening)
- Audio settings in game: all volumes full, 2 ch TV. Menu host mode/Treasures make no difference.
- Engine: Sony Scream with the BRB2 synth (brbDriver thread); Bluepoint audio config manager
  (biglib\audio\acm.cpp). Only 4 Ajm instances (codec AT9, flags 0x41 x3, 0x9 x1) decode the
  streams (music 4 ch, speech 1 ch); bank voices never reach Ajm (27 RIFF jobs per session, none on
  gunfire). common.bnk is intact in memory at 0x1000000000.
- The sound config text (sound1/config/ps4/prototypes.txt, banks.txt, reverbs.txt, environments.txt,
  mixers.txt) is NEVER opened and is not in memory. The load runs at startup:
  0x9bba5 -> 0x522d80 (acm 0x104c210) -> 0x519320/0x519410/0x5193a0/0x5159a0 (prototypes.txt)
  -> 0x515a50 -> file object 0x10fddf8 (enabled, callbacks installed: open = 0x8ef80) -> path builder
  0x8dfa0 (mode 1 loose override is off: 0x994188+0x3990 = 0; mode 2 = <root>/build/main/sound1/
  config/ps4/<name>) -> game FS 0x393760 (Fios). Without prototypes, every sound-proto request
  (footsteps, weapons, ambience) resolves to nothing; streams don't use protos.
- Next run (build 19:07): GOW3_AUDIO_TRACE also logs stat/access of sound paths;
  GOW3_FILE_TRACE=sound1/config. Find where the config open fails.
- 19:2x CAUSE FOUND (startup instruction breakpoints, GOW3_EXEC_WATCH, probe.c exec_watch_thread):
  open callback 0x8ef80 is called for reverbs.txt, mixers.txt, environments.txt and prototypes.txt
  (flags 1, type 1, mode 0). The path builds fine, but the Fios open (0x393760 -> 0x771a50) returns
  0x80820015 (SCE_FIOS_ERROR_READ_ONLY), mapped to EROFS 0x1e: `testb $5,%cl` at 0x8f1da turns flag
  1 into O_CREAT (0x40 -> Fios O_CREAT 8) on read-only /app0. Fios fails it without any kernel call.
  Fix: EBOOT_PATCHES "sound config files open read-only" (f6 c1 05 -> f6 c1 04). To be tested in
  gameplay; GOW3_EXEC_WATCH=515cc9 logs the loader result (expect rax 0).

## FIXED 14 October 2026 (19:3x): sound effects (footsteps, ambience, gunfire)
Confirmed by the user in gameplay. Log: all 4 config files open (reverbs 11405, mixers 405,
environments 10291, prototypes 204474 bytes); loader result 0 for each; Ajm now decodes bank
voices (794 RIFF jobs vs 27 before).
Fix: EBOOT_PATCHES "sound config files open read-only" (0x8f1da testb $5 -> $4).
Diagnostics left in code, off by default: GOW3_AUDIO_TRACE (now also stat/access of sound paths),
GOW3_FILE_TRACE, GOW3_EXEC_WATCH (startup instruction breakpoints, up to 4 guest offsets; hits log
registers and strings at rsi/rdx).
Still open: black bullet-hit circles; the Fios EIO crash seen once (17:47).

## 14 October 2026 (19:4x): dark interiors - fire and flashlights add no light; hair and cobwebs bright
- In the temple interior the braziers light nothing around them, the torch barely lights the walls,
  and hair, cobwebs and vines look unshadowed or bright.
- Wider readbacks (0x1030000000-0x1040000000, 0x112d200000-0x112d300000) changed nothing.
- Lights: cs 9bd0e806 builds per-light data (32 KB at 0x112d220200, including indirect args for the
  flashlight volume draws vs 1bf62085 / ps fe99f21a, 8a04b99b; those sample a D32 2048 shadow map and a
  512x512 cookie). cs 43b8ee5e (tiled lighting) loads 8 T# from light records with a GPU-computed
  index; SharpLocationFromSource -> UNKNOWN_LOCATION -> Image::Null -> null descriptor reads 0, so those
  lights are fully shadowed.
- Change (build 19:4x): resource_patching_pass returns 1.0 for reads of unresolvable T#s (lights apply
  without their per-light shadows); shader cache versions 8 -> 9. To be tested.
- Result: no visible change. 43b8ee5e has a single unresolvable image (its 8 T# dwords). The change was
  reverted (shader cache versions -> 10).
- Further facts for the dark-interior issue (open):
  - Flashlight volume indirect args are valid on the GPU (0x112d220200: 1230 indices, 0x112d220220:
    1200; the CPU view is stale zero).
  - Shadow map 0x1137458000 (D32 2048) is rendered by pass #15 (131 draws, depth less, no clear in the
    frame).
  - Indirect barrier is correct (dst AllCommands, MemoryRead|Write).
  - Depth-bias scaling never triggers.
  - Next: dump the light accumulation targets (0x113f8b0000 and 0x11408b0000 R16G16B16A16, written by
    the volume passes #20/#22) and the shadow map in this scene (frame dump diagnostic,
    guest-images.trigger) to see where the flashlight light is lost.
- 19:3x image dumps (launcher now always sets GOW3_FRAME_DUMP_TRIGGER/GOW3_DUMP_DIR =
  analysis/uncharted/dumps; write guest-images.trigger "addr size" lines, then touch trigger):
  in the dark room, the flashlight volume pass (ps fe99f21a) samples a 256x8 R16G16B16A16 ramp/LUT at
  0x1134ab0000 that is ALL ZERO (texture cache image flags 0x40; guest memory zero; GPU buffers zero).
  The light accumulation targets 0x113f8b0000/0x11408b0000 and the tiled output 0x11438f0000 are zero
  too. Shadow map 0x1137458000 (depth 0.18-0.79), cookie and normals look valid. Nothing writes the LUT
  in any capture; it is built at load/startup.
- GOW3_WATCH_RANGE now also logs render targets (BeginRenderingFull) and storage images (BindTextures);
  probe default 1134a00000:1134b00000 to find the LUT writer on the next launch/load.
- 19:4x narrowed GOW3_WATCH_RANGE (1134ab0000:1134ad0000): NO GPU-side write (render target, storage
  buffer/image, DMA, fill, WRITE_DATA) to the LUT in a whole session (startup, load, dark room), and the
  LUT is still sampled at 0x1134ab0000 (ps fe99f21a). Direct memory aliases share one section, so
  aliasing is not the issue. Next: startup data watchpoints on the LUT texels (GOW3_EXEC_WATCH now
  accepts w:<absolute address>, 8-byte write watch; GOW3_EXEC_WATCH_SECONDS, default 600).

## 14 October 2026 (20:0x): Uncharted 2 and 3 from the Collection menu

Symptom: picking Uncharted 2 or 3 in the menu closed the game. Four earlier boot logs show
`STOP: first unsupported PS4 import: JoBqSQt1yyA#libSceSystemService`, caller 0x14c7fd.

Cause:
- The Collection is three executables. The menu (eboot.bin, also Drake's Fortune) calls
  `sceSystemServiceLoadExec("/app0/big2-ps4_Shipping.elf"` or `big3`, with argv
  `{"eboot.bin", "ispartofuccollection"}` (string table 0x7778b0/0x7778d0 and 0x7a95ba/0x7a95c4;
  `"simulateplaygo"` is appended when a debug flag is set).
- The runtime had no LoadExec, so it exited.

Changes (not yet tested in game; the game was running during the build):
- runtime_services.c `load_exec`:
  - Writes the path and arguments, one per line, to GOW3_LOADEXEC_FILE (analysis/uncharted-user/loadexec.txt).
  - Exits with code 30. Without a handoff file it exits with 31 and a STOP line.
- probe.c: new `--guest-path /app0/<exe>` and repeatable `--guest-arg` options build guest argv (previously fixed to `/app0/eboot.bin`).
- tools/probe_uncharted.py loops:
  - On exit 30 with a handoff file, it prepares the requested executable and relaunches with those arguments. The same path handles U2/U3 back to the menu.
  - U2/U3 started directly (Start-Uncharted2.cmd / Start-Uncharted3.cmd) get the menu's arguments.
- U2/U3-only imports, mapped in import_names.inc and implemented:
  - POSIX open/read/write/lseek/close/fsync through libScePosix (existing implementations).
  - fcntl, select and sockets (network down).
  - pthread_condattr_init, scePthreadGetthreadid.
  - NpSignaling GetConnectionInfo/TerminateConnection/GetPeerNetInfo, NpScorePollAsync.
  - A `--check-imports --cpu-only` diff now shows nothing missing in U2/U3 that is not also missing in U1.
- The 2 s GPU frame-wait patch also applies to U2 (image 0x718886) and U3 (0x79f316); the code around it is byte-identical to U1's.
- The U1 sound-config patch has no counterpart: U2/U3 have no prototypes.txt.
- The U1 dark-interior diagnostics (GOW3_WATCH_RANGE, GOW3_EXEC_WATCH) are only set for eboot.bin.

Result (14 October 2026, 20:2x):
- The first test showed a grey screen at 60 FPS. The game loop ran, but nothing loaded and every draw was a fullscreen pass.
- Cause: the launcher passed argv as `/app0/big2-ps4_Shipping.elf eboot.bin ispartofuccollection`.
  - The U2 option parser (0x637250, called from 0x14972c) appends unknown arguments to a list at +0x3a10 and clears +0x4975.
  - So "eboot.bin" was taken as something to load, and the title screen never started.
- LoadExec's argv is the new program's complete argv, with argv[0] "eboot.bin". The launcher now passes it that way.
- U2 and U3 both reach their "Press X to Begin" title screens at 60 FPS (no input sent).
- Gameplay is not tested yet.
- Diagnostics used, in the scratchpad: walk.py (thread frame chains), fibers.py (sceFiber objects and the call chain of each suspended fiber), busy.py (non-idle guest call chains).
  - U2's game loop runs as job 0x14e020; its frame counter is at image+0xebd4e0.

## 14 October 2026 (20:3x): Uncharted 2 new game froze on the loading screen (dagger icon)

The game loop stopped at frame 0x501. An lldb backtrace of all threads (`lldb -p PID --batch -o "thread backtrace all"`) showed a deadlock:
- The GPU command thread: Rasterizer::Draw → GetGraphicsPipeline → QueueShader → `capture_job.Wait()`.
- The ShaderCapture worker running that job: SRT walker (JIT) read of 0x1038e0cd80 (GPU-written constants) → read fault → Rasterizer::ReadMemory → BufferCache::ReadMemory → `liverpool->SendCommand<true>` (waits for the GPU thread).
- Every other thread that faulted, plus direct_unmap → UnmapMemory → InvalidateMemory, queued behind the GPU thread.

Fix:
- PipelineCache::capturing_for_gpu_thread is a thread_local, set inside a capture job that the GPU command thread waits on.
- Rasterizer::IsGpuSideThread() returns true for that thread, so the fault takes the inline readback path (assume_locks) that a GPU-thread fault would take.
- This is safe because the GPU thread is blocked in Wait() for the whole job.

Not yet confirmed in game.

Second hang (20:34, caught by scratchpad/hangwatch.py, which runs lldb when U2's frame counter stops for 8 s):
- A draw-preparation worker in GetProgram held `programs_mutex` (shared).
  - Inside its speculative section (runtime_fault_recover set), ParseFetchShader read a GPU-modified page.
  - The read fault went to gow3gpu_handle_fault → ReadMemory → SendCommand, which waits for the GPU thread.
- The GPU thread was waiting for `programs_mutex` exclusively (GetProgram line 1245).
- Fix (probe.c, both the Windows and the POSIX path): during a speculative read, a fault skips GPU page handling and takes the recovery path. The draw-prep result fails and the GPU thread does that draw itself.

## 14 October 2026 (20:4x): U1 dark interiors - lights FIXED by synchronous shader compilation

Cause:
- The 256x8 R16G16B16A16 LUT at 0x1134ab0000 (sampled by the flashlight volume pass, ps fe99f21a/8a04b99b) is rendered once at startup by ps 2d7325a4 (render target 128 KiB).
- With async_shaders=1, PipelineCache::GetGraphicsPipeline returns nullptr for a pipeline that is still compiling, and that draw is skipped.
- Pipelines are not warmed up at start ("on demand"), so this one-time draw was skipped every session.

Fix (tested by the user 20:4x: the torch and flashlight light the room):
- The launcher sets GOW3_ASYNC_SHADERS=0 for eboot.bin. This overrides async_shaders in probe.ini.

Still open:
- Hair and cobwebs are bright in dark rooms.
  - Hair is pass #33 (vs 58b25301, ps cc2d0c16; alpha blend). Cobwebs and other transparents are pass #35.
  - Both sample the D16 2048 shadow map 0x11352d0000 (pass #5: a depth-ALWAYS full-screen draw first, then 68 casters) and the light accumulation 0x113f8b0000.
  - Suspect: the sun or directional term is unshadowed for forward-lit objects.
  - The guest image dump now supports D16Unorm.
- Frame drops to 45-57 FPS in the temple.
  - A host sample of the GPU command thread: about 49% is in BufferCache::DownloadMemory → Scheduler::Wait, called via SendCommand from BufferCache::ReadMemory. Guest CPU threads read GPU-modified pages, and every fault waits for the whole GPU.
  - GOW3_FAULT_STATS=1 (page_manager.cpp, launcher default for now) prints the counts, the hottest guest RIPs and the 64 KiB windows every 5 s.

## 14 October 2026 (21:1x): temple frame rate and bright hair/cobwebs, measurements

Frame rate (GOW3_FRAME_STATS):
- The temple room runs a steady 45-49 FPS (21 ms median) with 0 shader compiles.
- The GPU command thread is never idle and is blocked on GPU ticks about 50% of the time.
- The DMA sync of the tiled lighting shader (BufferCache::SynchronizeDmaBuffers before each cs 43b8ee5e dispatch) uploads about 3 GB per 5 s, all CPU-dirty resident memory.
  - New live modes via GOW3_DMA_TRIGGER (analysis/uncharted-user/dma.trigger): "full" (default), "off", "pointers <KiB>" (windows around guest addresses in the stage's user data and flattened SRT).
  - Measured: full 45 FPS, off 48, pointers 48 (about 170-260 MB per 5 s). So the sync is about 3 FPS of the loss. The rest is GPU time.
- Next: GOW3_GPU_PROFILE=1 (now a launcher default) for GPU ms per pass.

Hair and cobwebs:
- The D16 2048 map 0x11352d0000 is cleared to 0 inside a rectangle (a z=-1 TriList quad, DX clip space, z clip off, viewport z 0..1). Every LESS caster fails against 0, which is also what a PS4 does.
- All shadow lookups use image_sample_c_lz with compare LESS. Hair ps cc2d0c16 (hero-default.pak), cobweb 7187fa25 (catacombs-1.pak) and eddie 3d0b0bda each have one.
- Live compare overrides ("cmp never|always|normal|border-black|border-white|edge" in the same trigger file):
  - never: the cobweb and hair go dark.
  - always: walls are sunlit too.
  - normal: walls are dark, but the cobweb and hair are lit.
- So the hair/cobweb lookups land outside the cleared rectangle (border 65535) or beyond the texture edge. The compare-sampler setup (clamp/border) is now logged once per setup.

## 14 October 2026 (22:0x): bright hair/cobwebs - cause found: MUBUF ADDR64 loads were unimplemented

- The D16 2048 sun shadow map 0x11352d0000 is a 4-slice array, one slice per cascade (DB_DEPTH_VIEW slices 0-3).
  - Slice 0 is all 0 in the temple: the roof is pancaked to the near plane, which is correct.
  - Slices 1-3 have content. Dumps now include every array layer (_L<n> files).
- Hair (ps cc2d0c16, hero-default.pak), cobweb (ps 7187fa25, catacombs-1.pak) and eddie (ps 3d0b0bda) look up shadows with image_sample_c_lz (DA=1, compare LESS, clamp to edge).
- Their cascade data (192 bytes per cascade) is read with BUFFER_LOAD_DWORD/X4 in ADDR64 mode:
  - vaddr v[0:1] = s30 + 192*cascade; V# s[64:67].
  - The translator ignored mubuf.addr64 and treated v0 as a buffer offset, so the matrices were garbage and the lookups landed in lit texels.
  - The walls' shader does not use ADDR64, so walls were right.
- Fix (vector_memory.cpp BUFFER_LOAD): for ADDR64 untyped dword loads, address = V#.base + v[vaddr:vaddr+1] + soffset + inst offset, read with ReadConst (the direct memory path; the shader becomes a DMA user).
  - Shader cache versions 10 -> 11.
- Tooling: gpu target `gcn-disasm` (tests/gcn_disasm.cpp) disassembles GCN binaries with the recompiler's decoder. Shader binaries come from the game packs by their OrbShdr footer hash.

Hair/cobweb FIX confirmed (22:1x, user screenshot):
- The cobweb is dark like the room and Nate's hair is natural. No flicker: 6 consecutive screenshots give the same cobweb-region brightness.
- The first ADDR64 build flickered. DMA reads of pages that the BDA page table did not map yet return 0 and are mapped only after the submission (FaultManager), and per-frame data lives on fresh pages.
  - Fix: DMA sync mode "pointers" (launcher default GOW3_DMA_SYNC="pointers 256") calls ObtainBuffer on 256 KiB windows at each guest address in the DMA stage's user data / flattened SRT data. The windows become resident and current before the draw.
  - The full per-draw sync is gone: 490 -> 75 ms per 5 s on the GPU thread.
- Flatten pass: ReadConsts with a per-lane (non user-data) base are left dynamic, and SimplifyReadConstAddressAdd checks the base arity.

Temple frame rate (47-49 FPS), cause:
- About 1000 GPU downloads per 5 s with a full GPU wait each (buffer_cache.cpp DownloadMemory; about 2.5 s per 5 s).
- They are all CPU writes into GPU-written memory in 0x1038d00000 / 0x1038e00000 (GPU constants of cs 2d215c3b), 0x1036200000 and 0x1061e00000.
- They come through Rasterizer::OnWriteFault -> InvalidateMemory, with the download run on the GPU thread (SendCommand).
- Fix in test: BufferCache remembers 1 MiB windows where this happened (IsCpuWriteHot). GPU storage writes up to 4 MiB in those windows join the per-submission batched download (SyncSmallGpuWrites), so the CPU's later writes find clean pages.
- Measuring tools: GOW3_FAULT_STATS (download stats by window), GOW3_PERF_DIAG (wait profile by caller), GOW3_FRAME_STATS, GOW3_GPU_PROFILE (now works for Uncharted: frames are marked at the display pass after a flush).

## 14 October 2026 (22:5x): temple frame rate - lazy CPU writes into GPU-written pages (in test)

Measured before the change (brazier room, 46-52 FPS):
- About 800 CPU write faults per 5 s land in GPU-written pages. Each one downloaded a 512 KiB window with a full GPU wait (buffer_cache.cpp DownloadMemory, about 2.5 s of waiting per 5 s; "GPU ticks" blocked about 50%).
- None of the faulting writes touch GPU-written bytes. They hit other bytes in the same page: about 300 GPU-written bytes per page.
  - 0x1036221bf4..c0c: GDS-to-memory DMA copies.
  - 0x1061e00000: 1 KiB storage writes of cs 6a6422a9.
- The 2d215c3b scalar-copy HLE (vk_shader_hle.cpp) had already removed the 0x1038d/e stalls.

Change (BufferCache::TryLazyWrite and LazyPage; GOW3_LAZY_WRITES=0 turns it off; relaxed readbacks only):
- A write fault whose bytes don't intersect gpu_modified_ranges unprotects the page and skips the download. The guest copy of the page's GPU-written bytes stays stale and is recorded.
- Uploads (SynchronizeMemory, through ForEachLazyUploadPiece) leave out recorded bytes whose guest copy still matches the record, so the GPU values in the arena survive.
- A byte that differs from its record was written by the CPU after the GPU, and the CPU value wins:
  - an upload sends it and subtracts it from gpu_modified_ranges;
  - a download (WriteBackGpuData) doesn't overwrite it.
- Downloads erase the records of their pages.
- The "Download stats" line now adds "lazy writes (no download)" and the number of GPU-written bytes the CPU overwrote later.

## 14 October 2026 (23:1x): async readbacks, 60 FPS in the temple; treasure-pickup crash fixed (in test)

Result of lazy writes:
- The DownloadMemory waits are gone.
- The next limit was the per-submission readback of small GPU writes (SyncSmallGpuWrites -> DownloadRanges, ~10 ms Finish per frame, 44 FPS with ~2600 draws per frame).
- Turning that readback off ("off" trigger) didn't help: the CPU then wrote GPU-written bytes about once per frame (0x1036200000), each a full wait.

Async readbacks (BufferCache::DownloadRangesAsync; GOW3_ASYNC_READBACK=0 waits instead):
- The copies are recorded at submission end into a held HostCached staging buffer.
- PollReadbacks (per BindResources and at OnSubmit) writes them back once the tick passes.
- Until then the pieces stay in gpu_modified_ranges. `rewritten` holds later GPU writes, which stay GPU-modified.
- A CPU write fault on bytes whose newest data is in a readback waits for that tick only.
- Synchronous downloads complete overlapping readbacks first.
- User result: steady 60 FPS (frame median 16.65 ms, ~3000 draws per frame).

Crash on picking up a treasure: STOP in ir_emitter Pack4x8 "Unsupported 4x8 number format: Float".
- cs 8f067540 bound garbage V#s (Clamped size errors).
- Likely cause: the 2d215c3b scalar-copy HLE checked GPU-modified per page. Lazy pages and pending readbacks keep page bits set longer, so copies went to the GPU, and the SRT walker read their output a frame late (uninitialised for a new effect).

Fixes:
- The HLE checks bytes (BufferCache::IsRangeGpuWritten) and unprotects dst itself (PrepareHostWrite, lazy snapshot of other GPU bytes in the page).
- The pack/unpack helpers in ir_emitter.cpp log and fall back to the default format instead of UNREACHABLE.
- GOW3_FAULT_STATS prints "Scalar copy HLE (5 s): N on the CPU, M on the GPU".
- Launcher: GOW3_GPU_PROFILE and GOW3_WATCH_RANGE are off (the profiler splits render passes).

## 14 October 2026 (late): Uncharted launcher, frame-rate unlock, render resolution

- **Launcher:** launcher/uncharted_launcher.pyw (Tkinter + Pillow), started by Start-Uncharted.cmd and Uncharted.lnk.
  - Game cards for U1/U2/U3; quality presets Low (720p/30) / Medium (1080p/60) / High (1440p/60) / Ultra (1440p, unlocked); Display, Performance and Advanced tabs.
  - Writes probe.ini (shared with the in-game menu); launcher-only options go to %APPDATA%/Uncharted Launcher/launcher.json.
  - Env passed to tools/probe_uncharted.py: UNCHARTED_DIAGNOSTICS, GOW3_VBLANK_HZ=0 for limits above 60, UNCHARTED_RESOLUTION, GOW3_DRAW_PIPE / GOW3_VK_RECORD_THREAD (multithreaded rendering, experimental, off).
- **Game runs as gow3_pc/out/Uncharted.exe**, copied from gow3-probe.exe at launch. The exe icon, the window icon (sce_sys/icon0.png) and the title are Uncharted.
- **In-game menu:** the GoW tabs and rows are removed (cheats, orbs, enemy health bar, engine frame rate, resolution patches, game hooks). FPS limits are 30/60/120/144/165/240/unlimited, without the engine-fps cap.
- **Frame-rate unlock:** the present thread uses the menu limit; above 60 needs a 480 Hz vblank (GOW3_VBLANK_HZ=0). Untested in game (game speed).

Render resolution (tools/probe_uncharted.py resolution_patch, UNCHARTED_RESOLUTION=WxH; all three games have the same code):
- The display settings constructors and the 1080p/720p mode switch store the size in many fields:
  - 1920x1080 at +0x08/+0x18/+0x24/+0x38
  - tiles +0x40 = w/8, +0x44 = h/4
  - a larger size at +0x30/+0x34 (2560x1216)
  - packed movabs 0x43800000780
  These are 33 stores in total. There is also a generic SetResolution(w, h, aspect) that computes the same fields.
- Reporting a 4K display (sceVideoOutGetResolutionStatus) has no effect: the size is hard-coded.
- Memory table (32-byte entries {id, parent, u64 size, u64 align, 0}):
  - Render Target Heap: id 0x85, parent 6. 497 MiB in U1/U2, 561 MiB in U3.
  - GPU (garlic) block: id 3, parent 2. 2988 MiB in U1/U2, 3052 MiB in U3.
  - Both grow by heap * (pixel factor * 1.1 - 1), rounded up to 64 MiB. A non-aligned garlic crashed right after the onion mapping.
- The table check `lea rsi,[rbp-0x20]; movabs rdx,0x120000000` expects the direct blocks to total 4608 MiB, so it's raised by the extra too.
- The runtime extends the game's 5056 MiB search end to GOW3_DMEM_MB (runtime_memory.c).
- **Tested** (boot to title, 45-50 s, no asserts): 720p and 1440p in U1; 1440p in U2 and U3.
- **Limits:**
  - The "GPU Mem" heap is the whole garlic block, and its size is cut to 32 bits: at 4K (4716 MiB) it became 620 MiB, and the 368 MiB sub-heap 0x87 failed. Garlic must stay under 4 GiB.
  - At 1800p a second heap, "Render Target" (id 0x84, 139.77 MiB), ran out (a 45 MiB request).
  - 4K needs the 32-bit cut fixed. The heap names are set by static constructors around 0x4123xx (GPU Mem object 0xd93c40), and the size comes from a separate init that hasn't been found yet.
- MediEvil (reference/medievil 3a16e0e) did 4K with the same method: a bigger framebuffer pool plus the address-range checks in its free paths.

## 15 October 2026 (00:xx): U2 crash (fiber stacks), native 4K

**Uncharted 2 in-game crash** (exit 0xC0000005, nothing logged; WER: fault at 0xb8ecf3e = guest 0x4ecf3e)
- Crash site: a `ChooseJob` worker's `mov [rsp+0xc0], rax` hit a write-protected stack page. With the stack unwritable, Windows can't push the exception frame, so it kills the process silently.
- Fiber stacks come from game memory (sceFiber contexts). The DMA pointer windows (256 KiB around any dword pair that looks like an address) can cover them.
- Fix: gow3_fiber.cpp registers every fiber context (Initialize/AttachContext; Finalize unregisters), and AddressSpace::Protect (gow3gpu.cpp Gow3KeepWritable) never write-protects those ranges.
- Tried first and discarded: a no-protect window sync. Re-uploading writable pages cost ~1 GB/s.
- Image layout: the runtime image is the end of boot-linked.bin (size at header offset 8) at base 0xb400000. Guest offsets are image offsets. **boot.bin is NOT the loaded layout.** Helper: scratchpad re/img.py.

**4K**
- Memory areas: entry 2 onion = context 5 ("CPU Mem"), entry 3 garlic = context 6 ("GPU Mem"). MemoryArea (memory-area-ps4.cpp) holds one page id per MiB in a 4096-entry array, so one area can't exceed 4096 MiB. MemoryArea::Initialize asserts MiB-aligned sizes. Each area starts on a 4 GiB boundary.
- Fix: above the garlic limit the Render Target Heap (0x85) moves to the onion area (context 5, rounded to 64 MiB). Garlic's rounding goes to the second RT heap (0x84), which also grows with the pixel factor.
- Boot-tested at 3840x2160 (title screen, 50 s, no asserts): U1 and U2. Gameplay is not tested.
- U3 at 4K, three attempts:
  - First: "HALT: Command context dcb full! Increase size!" (ndgi-orbis.cpp ComputeContextBufferFullCallback). The compute contexts are set up by the function that installs that callback; buffer sizes are "Deform Entities" 16 KiB and, U3 only, count*0x1400+0x1c00 capped at 0xd000.
  - Buffers enlarged x4: corruption crash (wild pointer at guest 0xe5260b).
  - Buffers enlarged x2: corruption crash at 0x55bc10.
  - Likely more buffers sized for 1080p (tiled lighting lists?). U3 is clamped to 1440p in probe_uncharted.py for now.

## 15 October 2026 (00:3x): resolutions above 1080p fail in gameplay; presets back to 1080p

User sessions:
- **U2, 1440p, 60 FPS:** played for minutes, then the game assert `m_lookAt.X() == m_lookAt.X()` (NaN) in UpdateFocus, player-base.cpp:1101.
- **U1, 4K Ultra:** assert `offsetTableIndex != -1 && offsetTableIndex < GenericOffsetTable::s_totalValidOffsetTables` (BP/Shared/TGenericOffsetPtr.h:100): Bluepoint's offset pointer tables don't cover the Render Target Heap after it moves to the onion area. Just before: 32k guest read faults per 5 s at 0x10ecxxxxxx (that heap).
- **U3, 1440p:** corrupted pointer while loading into the game (guest 0xe5dca2, same area as the 4K corruption).
- The green cutscene at 1440p is probably the same class of problem.

Changes:
- The launcher presets all render at 1080p. High and Ultra use FSR 1 to the screen; Ultra unlocks FPS with 2 frames queued.
- Resolution above 1080p is labeled experimental. Config version 2 resets a saved resolution to 1080p.
- U3 is clamped to 1080p in probe_uncharted.py.

Next for >1080p:
- CPU readbacks of render targets that assume the 1080p layout (look-at NaN).
- Buffers sized for 1080p (U3).
- BP offset tables, for the onion move.

## 15 October 2026 (01:00): U3 crash cause, U3 hair shadow flicker

**U3 "dcb full" crash at the London pub, even at 1080p**
- Bisect with the user: all three new mechanisms off: OK. Only GOW3_ASYNC_READBACK off: OK. So the cause is the asynchronous readbacks. U3 runs with GOW3_ASYNC_READBACK=0 (probe_uncharted.py).
- Planned fix: read-protect pages with in-flight readback pieces; a guest read waits for that readback only.

**U3 hair lit for a frame now and then (sunlit museum)**
- Frame dumps: about 1 in 8 frames loses the window-frame sun shadow on hair. The ADDR64 hair shaders (ps 5d1bfb2d / 9e3f2c9d / 47f0eee5 / b107ecc9, pass 29) read cascade data at (s42:s43) + v2.
- Ruled out: DMA page faults (0, new GOW3_FAULT_STATS "DMA page faults" line), full DMA sync, compiles and skipped draws.
- Cause: DMA reads are invisible to the barrier tracker. The window uploads recorded in SyncDmaMemory (end of BindResources) never set needs_barrier, so a read could see a copy mid-flight, and later copies into the arena had no barrier against the draw's reads.
- Fix: needs_barrier |= IsBufferAccessed(arena, whole) for the window arenas, and ResetBindings registers a shader read of those arenas (dma_read_arenas). Built 00:59; the user's test is pending.
- The U3 hair shaders were extracted from the actor33 packs (young-drake.pak etc.; scratchpad find_shaders.py and u3shaders/).

## 15 October 2026 (night): update 1.02, resolution causes, movies above 1080p, plan

Plan and test list: analysis/PLAN.md.

- **Update 1.02** (`utndc.v1.02.pkg` → `game/CUSA02344-patch`, metadata from the PKG entries): `tools/probe_uncharted.py` finds `<game folder>-patch|-update` (or UNCHARTED_UPDATE) and runs a linked view (gow3_pc/scripts/game_update.py; fixed a stale link record that kept the view from being removed). All three executables were recompiled. Patches are hex patterns with `??` wildcards and per-version variants.
  - GPU wait: in 1.02 the 2.0 s timeout jumps straight to `int 0x41` (U1 0x4fdf96). Same bytes in U2/U3 (`c5f258c0 c5f82e05 ???????? 7710 f390 498b00`).
  - Resolution: the 1.02 constructors no longer store 1920/1080; only the display settings init (U1 0x4d0710, global 0xe76e74: +0x08/+0x18/+0x24/+0x38 sizes, +0x68 60.0f) does, with [rax]. Total check uses lea rsi,[rbp-0x30] in U2/U3.
- **Frame timing**: dt = rdtsc delta / TSC frequency (U1 1.02 0x9d419), clamped by "Minimum Framerate" (clock update 0xfac70); `forcefps <n>` overrides. Game speed is right above 60.
- **>1080p crash causes**:
  - U2 1440p NaN and U3 1440p pointer crash: async readbacks were on (584/230 in flight).
  - U1 4K "offsetTableIndex != -1": GenerateOffset (U1 1.02 0x662ae0) packs pointers as table<<31 | offset; tables fixed at [0, 2 GiB) and [0x1000000000, +2 GiB) (set at 0x7e156). The moved Render Target Heap is created before the level heaps (0x82, 0x80), so they land past 2 GiB (failed pointer 0x10e92cbd1c, animation loader). 4K frame capture: RTs at CPU area +1008..2393 MiB. Neither 1 MiB alignment nor context 8 (null heap at boot) moves it. U1 renders at most 2880x1620 (move=False) and upscales.
  - The 13 Oct 1080p assert of the same kind: pointer 0x8007c301a, a rodata string, with the image at 0x800000000 (before --low-image). Table 0 is needed.
  - U3 1.02 at 4K: CPU area mapped 100 MiB larger than the table (4132 MiB wrapped to 36 MiB). 128 MiB headroom; margin falls back 1.1 → 1.05 → 1.0.
  - Boot-tested: U1 1440p/1620p, U2 4K, U3 4K.
- **Movies above 1080p** (green, tiled): the movie player (movie-rendering.cpp, U1 1.02 0x4fb7b1) builds the luma/chroma textures with the display size (+0x08/+0x0c) and the chroma offset W*align(H,64) from +0x24/+0x28; frame buffers are fixed at kSizeFrameBuffer = 4 MiB (movie-player.cpp). Now: gpu/shim/gow3_movie.cpp + videodec2 write a copy scaled to GOW3_MOVIE_SIZE into flexible memory at 0xf000000000 (runtime_map_guest_memory) and ImageInfo reads the planes there. The conversion shader (cs c85d9816) samples at (x+0.5)/1920, (y+0.5)/1088 as literals: shaders holding both literals get them rescaled in an IR step (recompiler.cpp) and a cache key with the display size (regs_shader.h GetParams, Gow3Movie::ShaderKey). Rows past the frame height repeat the last one (1620p). Verified at 1440p and 1620p: same picture as 1080p.
- **Low-end reference**: integrated Radeon (GOW3_GPU_ID=1), U1 title: 21 FPS at 1080p, 44 at 720p.
- **Red zone** (shadPS4 #5038): Windows exception frames overwrite the SysV red zone. About 340 red-zone blocks per game, none at known crash sites.
- **Safe async readbacks** (GOW3_ASYNC_READBACK=1; =2 the old unprotected mode): PendingReadback.watched = merged pages of its pieces, read-protected (PageManager read watchers) from the first CompleteReadbacks after its submission (protecting at record time let the GPU thread fault on its own unsubmitted tick); a read fault on such a page completes readbacks up to that tick and returns (relaxed). Fiber stacks now keep read access too (AddressSpace::Protect). U2/U3 boot with it (U2: ~900 readbacks / 5 s, ~10 reads waited). U1 exits 0xC0000005 while loading right after 0x119de2e000 +0x61000 is protected, with no access violation reaching the vectored handler (GOW3_FAULT_TRACE=1 shows the first 64): probably a stack in that range. Launcher option per game, forced off for U1.
- **Display modes** (window.cpp): the window was created at 1920x1080, so on a 1920x1080 screen "Windowed" put the title bar off screen over the taskbar; exclusive "Fullscreen" minimized itself whenever the game window lacked the focus (SDL's default), which it often did when started from the launcher. Now: windowed is 16:9 within 85% of the usable desktop, centered, and restored when leaving fullscreen; SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS=0; SDL_RaiseWindow on each mode change; the launcher calls AllowSetForegroundWindow(ASFW_ANY) before starting the game. Verified (GetWindowRect): windowed 1559x876 centered with caption, borderless and fullscreen 1920x1080 in the foreground. UNCHARTED_CONFIG_FILE points tools/probe_uncharted.py at a test settings file.
- **U1 card started U2** (user, 15 Oct): the U1 card started eboot.bin without arguments, i.e. the Collection's game selection, whose cursor sits on the last game played; one Cross started that game (LoadExec big2). eboot.bin's argument parser (U1 1.02 0x5cf1a9) knows "ispartofuccollection" (sets +0x39c1 and clears +0x39b0, as the Collection starts a game): with "eboot.bin ispartofuccollection" it opens Drake's Fortune's own title. That is now the default (probe_uncharted.py --collection-menu for the selection). Also: typing in another window while the game window took the focus pressed buttons (Space = Cross); GOW3_KEYBOARD=0 turns the keyboard input off for test runs (default on).
- **Keyboard and mouse** (15 Oct): gpu/shim/gow3_mouse.cpp captures the mouse (SDL relative mode) while the game window has the focus and no menu or text dialog is open, and hands movement and buttons to runtime_pad.c (gow3gpu_mouse_take). Mouse speed becomes the right stick (smoothed ~25 ms, dead-zone offset 0.15, GOW3_MOUSE_SENS, GOW3_MOUSE_INVERT); mouse buttons and the wheel map to PS4 buttons (GOW3_MOUSE_MAP), keys to buttons and stick directions (GOW3_KEY_MAP: ls_*/rs_* names; the first entry of a button replaces its defaults, more entries add, an empty name unbinds). New PC defaults: WASD, mouse look, RMB L2, LMB R2, Space Cross, LCtrl Circle, F Square, R Triangle, Q/X1 L1, E/X2 R1, LShift L3, V/MMB R3, Esc/Enter Options, 1-4 d-pad, wheel d-pad right/left. Launcher CONTROLS tab per game. Launcher: YouTube/GitHub links, own AppUserModelID (taskbar icon).
- **DLSS** (15 Oct): DLSS 5 (neural rendering) has no public SDK. NVIDIA/DLSS on GitHub is at 310.9.1 (8 Sep 2026; the NGX header keeps feature 18 as Reserved18) and Streamline at 2.14.1. The community DLSS 5 tools use leaked or driver-store nvngx_dlssnr.dll through a shim that bypasses NVIDIA's caller check (0xBAD00002), only on D3D12, and they are not redistributable, so they are not used here. The possible path is DLSS 4.5 Super Resolution/DLAA through the existing bridge (vk_dlss.cpp, gpu/dlss_bridge, packaging/windows/build_dlss.sh, official SDK). It needs per-game hooks. In capture frame_20261014-104215_2 (1080p), the scene HDR color is RGBA16F 0x1133ab0000, written by a 716-draw pass with scene depth D32S8 0x1130438000. The post chain then runs: 960x540 RG16F motion/velocity-like targets, 480x270 bloom, R8 1080p, and RGBA8 1080p before the sRGB display buffer. Camera constants and jitter are still to be found.
- **Uncharted 2 DLSS/DLAA** (17 Oct): camera found. It is not in a buffer the GPU reads by address: the CPU copies it into the flat user data of every scene vertex shader, at a per-shader offset (vs 3797ed97 float 58, 4dd98c91 48, c6c32866 44, ...): world-to-view 3x4 rows, then 0 0 0 1, then the projection rows (-0.8391 0 0 0 / 0 1.4917 0 0 / 0 0 1 -0.24 / 0 0 1 0 at 1080p: clip w = view z, depth = 1 - 0.24 / z, no game jitter). The game's own camera position sits before it (vs 3797ed97 float 54) and equals -R^T t. gpu/.../flat_camera.h finds the block in any shader (orthonormal rows, fixed zeros, perspective last row; the sun shadow's orthographic camera never matches); GOW3_CAMERA_SOURCE=flat takes the first match of a frame on a scene-size D32 draw (the light pre-pass normals, depth 0x1132438000). Frame (capture frame_20261017-214933_3): pre-pass #0-4, sun shadow 2048 #6, opaque into HDR 0x1135ab0000 #21, transparents/fog to #31 (some against a copy of the scene depth made by cs 6a6422a9, which is jittered too), bloom downsample cs aec539f0 #40 (the upscaler trigger), game motion vectors 960x540 RG16F #38-39 (camera pass + 15 skinned draws), tonemap #51, display #54 and again #56: only the first display draw after scene drawing starts a frame. probe_uncharted.py enables it for Uncharted 2 when the settings file has upscaler=dlss/fsr3/fsr4/taa. Result on an RTX 5070 (chapter 1, 1080p, preset 0 = DLAA): "DLSS 1920x1080 -> 1920x1080, HDR input", 60 FPS; camera-motion debug view (GOW3_DEBUG_MOTION=1, toggle bit 23, |reprojected - current| x4) while panning: 97% of pixels below 24/255, the rest snow, Nate and the swinging carriage (object motion is not supplied yet; the game's half-res RG16F vectors at #38-39 are the candidate). Not done: other chapters, U1/U3, scaled presets (render < output), packaging gow3_dlss.dll/nvngx_dlss.dll, launcher option.
- **Uncharted 2 scaled DLSS presets** (17 Oct, render below output): the port's reduced scene targets (SceneTargets, from God of War III's live presets) now take the guest's native scene size from GOW3_SCENE_SIZE (probe_uncharted.py: the render resolution the patch gives the game) instead of 1920x1080: proxies for native-size targets, the upscaler output = that size (HDR reconstruction before the post, not RunScaled), jitter phases 8 x (output/render)^2 (guide 3.7.1.1), FragCoord of reduced passes rescaled by native/scene (push constant now in 1/16 pixels of a 1920x1080 reference, SceneResolution::PackCoord; scene-coord-test; Profile::scene_coord_version rebuilds older shader caches once). The scene starts reduced at the first draw with the camera's depth (light pre-pass), and those draws get the guide's LOD bias log2(render/display) - 1 (section 3.5; DLAA keeps 0). The bridge logs NGX_DLSS_GET_OPTIMAL_SETTINGS beside the port's size: Performance/Quality/UltraPerformance/DLAA identical (1920x1080, 2560x1440, 1280x720, 3840x2160 for 4K), Balanced 2258x1270 vs optimal 2227x1253 (inside the 1920x1080..3840x2160 range). Results (RTX 5070, chapter 1 carriage, 151 of 192 scene draws reduced):
  - 1080p output, Performance (960x540 scene): image correct, 60 FPS (vsync). Detail (mean |Laplacian| of a rock crop, three frames each): native 4.29, DLSS Performance 4.02, bilinear from 540p 1.99. Negative controls while panning: jitter sign flipped (toggle bit 26) 3.34 against 4.18 (-20%); zero motion vectors (new toggle UpscalerMotion, bit 58) 4.01 against 4.18, within run-to-run spread: the carriage scene pans too slowly to show it (to repeat in a faster scene). PSNR against native is not usable there: two native frames 1 s apart differ by 24-26 dB (the camera sways).
  - 4K output, GPU profile (GOW3_GPU_PROFILE, new segments "upscaler motion vectors", "DLSS evaluate (NGX)", "upscaler merge"): first try 13.55 ms/frame, 3.14 of it "scene proxy resolve D32S8 1920x1080" x124: the snow particles (vs 8b55e926) read the half-resolution depth natively in every draw. GOW3_SCENE_HALF_RES=0 (probe_uncharted.py) keeps half-resolution targets native: 10.69 ms. DLSS evaluation: preset M (the guide's Performance default) 2.80 ms, K 1.97 ms (guide table, RTX 5070 4K: 2.98 / 2.11 ms): the cost is the model. Presets are now pinned to transformer K for every mode (Dlss::TransformerPreset, GOW3_DLSS_MODEL=j/k/l/m overrides); since 18 Oct the model is the dlss_model setting (j/k/l/m, default k): launcher DLSS card, in-game menu Image tab (live: a new model recreates the NGX feature, FeatureDesc::preset), GOW3_DLSS_MODEL still overrides. U2 boot with m: "preset 13" in boot.log. The menu's DLSS mode is live between modes; on/off waits for a restart (pending_upscaler) because run.py prepares the upscaler at start. Against native 4K in the same session: GPU busy 9.45 vs 7.31 ms; DLSS +1.97, proxy resolves +0.57 (depth x4, colour x3: cs 43b8ee5e, ps 5e0b720c and the half-res depth downsample read the scene with integer loads), merge/fills ~+0.2, reduced scene passes ~-1.0. U2's reducible scene work at 4K is ~1.4 ms, so DLSS cannot be faster than native here; it is anti-aliasing at about native cost. Without the profiler, vsync off (one session, GOW3_PRESET_FILE live switches): native 103.6 FPS (GPU 75%, not GPU-bound), Ultra Performance 99.6, Performance 99.8 (GPU 89%), Balanced 99.0, Quality 97.8, DLAA 97.1.

## 15 October 2026 (morning): U2 treasure crashes, readbacks, torch

- **Torch/flashlight light nothing in U2 (FIXED, user-confirmed 09:4x).** Same cause as U1 on 14 Oct: the light ramp LUT (256x8) is drawn once at load and was skipped while its pipeline compiled in the background. Fixed generally in PipelineCache::GetGraphicsPipeline: draws whose color targets are all <= 256x256 pixels compile at once (sync_compile; a pipeline already queued is waited for). U1 still runs fully synchronous (GOW3_ASYNC_SHADERS=0).
- **SRT tables written by the GPU earlier in the same command stream** (Rasterizer::SyncSrtTables, hook Shader::g_srt_pre_walk in Info::RefreshFlatBuf; GOW3_SRT_SYNC=0 off): the walker's tables are decoded from its code (push rdi / mov rdi,[rdi+off] / reads / pop rdi), and tables with GPU-written bytes are downloaded before the walk. cs 7d7e7206 / a0b87e41 tables (12/240 bytes in per-frame rings 0x1100?da2c0) are GPU-written: ~15 syncs/s at the title, ~1 per 5 s in play.
- **Treasure effect garbage V#s (cs 7d7e7206, 8f067540):** the V# slots at table offsets 0x80-0xb0 hold floats for the treasure effect (CPU-written, not stale). Bound, written ones made huge ranges "GPU-modified" (game data overwritten by stale GPU copies: fg-draw-mgr `m_gfxEopTick` and mesh.h `iSubMesh < m_numSubMeshInstances` asserts) and a 3.8 GB arena ran out of device memory. Now a V# reaching past mapped memory (not num_records ~0) or spanning >= 1 GiB is bound as null ("Ignoring a garbage V#"). Only these two shaders ever had clamped sizes. DescribeSharpSource prints where a garbage V#/T# came from (table, offset, GPU-written state).
- **Async (safe) readbacks:** heavy U2 area 53-60 FPS (sync: 44). Fixed a recursion: WriteBackGpuData's lazy-page snapshot read a page this readback still watched; the fault completed the same readback again until the stack overflowed (silent exit in ___chkstk_ms). Lazy snapshots now read through the backing view (runtime_memory_read_backing / MemoryManager::TryReadBacking). Still open: with async on, both asserts above happened (before the garbage-V# nulling); retest pending.
- Per-frame sync off ("off" trigger) only moved the wait: three 1 MiB triple-buffered windows (0x1102600000/0x1102b00000/0x1103000000) get CPU writes into GPU-written bytes each frame (~10 ms waits each); 47 -> 50 FPS.
- Launcher writes the start script's output to analysis/uncharted-user/launcher-last-run.log.
- **U2 torch "points with the camera" (15 Oct): original behaviour, not a bug.** Live light records (image 0x1157760, 0x60 each: pos, dir, color, range/cone) and the frame capture agree: the player flashlight light (record 0, Flashlight process vtable image+0x100bca0, Active state update 0x69a190, attach locator via attach-system GetLocator 0x3b4620) sits at Nate's torch and its direction equals the camera's z axis exactly (ps a24da0bb slot 2 vs slot 0). The game aims the torch with the camera (script FlashlightFaceForward overrides it per scene). Precise readbacks changed nothing. Earlier "light origin at the camera" came from misreading the camera view-projection (vs 1bf62085) as the light matrix. Tool: scratchpad re/peek.py, re/flscan.py (ReadProcessMemory, read-only).

## 15 October 2026 (12:4x): frame-rate cap 120, launcher and in-game menu settings in step

- U1 at fps_limit=165 (140-160 FPS, 480 Hz vblank): the user found Nate falling outside the level (flat beige out-of-bounds geometry). The 120 cap of EmulatorSettings::GetFrameLimit never applied to Uncharted (the menu limit path used PresentLimit, which passed 165 through).
- Now: FpsLimits = 30/60/120 (gow3_graphics.h MaxFpsLimit); ParseFpsLimit maps 0 and >120 to 120, others to 60; PresentLimit caps at 120; default fps_limit 60. The launcher offers 30/60/120 only and Ultra uses 120 (it used the monitor rate, 165 here).
- Launcher showed 165 after choosing 60: the launcher works alone (sandbox click test), but (1) Ultra set the monitor rate and (2) the in-game menu's Save wrote the game's whole in-memory settings (165) over the launcher's change. Save now merges: keys unchanged in game since the last Load/Save keep the file's value (unit test: scratch settings_merge_test.cpp). The launcher re-reads the file when it changes (about once a second, and before Play) instead of writing its older values back.

## 15 October 2026 (13:xx-14:1x): U1 bullet impacts/blood and movie colours above 1080p FIXED (user-confirmed)

- **Bullet impacts, bullet holes, blood:** U1 1.02 casts bullet/decal rays on the GPU (meshraycaster.cpp: Cs_RayTrace 0x308607aa, Cs_RayTraceGatherHits 0xc92150ba). ProcessRayCasts (0x5460a0) records the batch with an EVENT_WRITE_EOP (data 1, interrupt) into the frame's GPU done counter (m_pGpuDoneCounter = the render frame's m_gfxEopTick); UpdateMeshRayCaster (0x547340) calls HandleResults (0x5475a0) when [counter+0x20]==0 and the decal callback OnRayCastResultsReady (decal.cpp) places holes/blood. Our EOPs are written when recorded, so the game read the hits before the GPU made them (every shot a miss). Fix: GatherHits' written buffers are collected (Rasterizer::eop_readbacks) and at the next EOP read back asynchronously with their pages read-protected (BufferCache::DownloadRangesAsync protect=true; a guest read before completion waits for that readback only). Tried first: synchronous download at the EOP (works, ~10 ms GPU wait per frame, 47 -> 29 FPS at 4K) and delaying the EOP until the readback (crash: assert GetRenderFrameParams(frameToFreeInstancesFor)->m_gfxEopTick in FrameBegin, fg-draw-mgr.cpp:234). Not the cause: the `is-japan?` check (0xfe0e0; settings+0x3994 region from a table, 0 = Japan) and the debug flags Enable/Render Decals (0x955a18/0x955a17), Show Blood Effects - all on.
- **Movie colours above 1080p** (grey picture, diagonal coloured lines, U1 at 1620p): GPU texture dumps (vk_presenter now dumps R8/R8G8 images too) against guest memory showed luma identical and chroma rows shifted 64 bytes per row: the chroma plane (1440 R8G8 texels at 2880 wide) is a linear-aligned texture whose rows the GPU reads at a pitch padded to 64 texels (1472, mips_layout pitch), while the scaled copy wrote 2880-byte rows. Gow3Movie::ScaledChromaPitch() = Align64(W/2)*2 bytes (2944 at 2880; unchanged at 1440p and 4K, where the earlier check passed). The decoder also writes each scaled frame through the backing view in one piece and invalidates it explicitly (no per-row faults).
- Frame capture + window capture method: capture.trigger (pass count) lists sampled images per pass; dumps/guest-images.trigger + dumps/trigger dumps them; scratch winshot_game.py / peek.py read the running game without input.

## 15 October 2026 (14:xx): U2 frame rate falling over minutes FIXED (user-tested)

- Symptom: in a static heavy U2 scene (about 3,400 draws per frame) FPS fell steadily, 42 -> 31 in 3-4 minutes, and process memory grew about 1 MB/s; CPU not throttling (9950X3D at 128% of base), renderer container sizes flat (new "Sizes:" lines every 5 s).
- Found with a new sampling profiler (gpu/shim/gow3_profiler.cpp, GOW3_PROFILE=<seconds>: suspends the GPU command thread about once per ms, writes module+offset counts to GOW3_DUMP_DIR/profile_<n>.txt; symbolize with llvm-symbolizer, image base 0x140000000; scratch symprof.py / cmpprof.py). BufferCache::ObtainBuffer grew from 7% to 26% of the thread, all of it in the page_writes prune (buffer_cache.cpp, from the shared-pages work this morning): a page's list grew by one entry per frame because entries were only dropped once their bytes stopped being GPU data, and every binding of a written buffer scanned the whole list with an interval-set check per entry.
- Fix: a write of a range already listed updates its tick; the prune drops entries GpuBytesInFlight ignores (finished, older than the last readback); a list still at 16 keeps its 8 newest. After: FPS flat at 40-41 for 4 minutes, ObtainBuffer 1.7% and flat, 3,152 entries constant. Process memory still rises about 0.8 MB/s (no effect on speed seen; to look at later).
- Also from this session: the U1 GPU ray-cast readback also runs in U2 (same shader c92150ba, 1 KiB ranges, about 40 per second); fence labels written through the backing view are now excluded from pending readbacks (BufferCache::NoteCpuWrite) after one U2 crash with the FrameBegin m_gfxEopTick assert (no label was later seen inside a readback; the crash did not recur in three sessions). Compute ReleaseMem in order: compute drains about 1,000/s, down from about 34,000/s.
- **U2 Nate invisible / floating (15:xx) FIXED:** bisected with test-env switches (ray-cast readback, torch patches, SRT sync, async readbacks ruled out): the scalar-copy HLE of the 64-wide copy shader 6a6422a9 (added this morning) made Nate float above the ground or vanish (camera inside him). Its shader semantics are right (dst[i] = src[i], i = group*64 + thread < s6); the CPU copy runs when the command is processed, earlier than the console's GPU, and the data it copies (the player's) is completed later. Now 6a6422a9 runs on the GPU by default (GOW3_SCALAR_COPY64_CPU=1 restores the CPU copy); 2d215c3b keeps its CPU copy (since 14 Oct). A per-frame "source changed after the copy" check was tried and removed (it flagged 282 per-object blocks and did not fix Nate). GOW3_SCALAR_COPY_GPU=1 still keeps both on the GPU; GOW3_EOP_READBACK=0/2 switches the ray-cast readback off / to synchronous.
- **U2 crash with multithreaded rendering (assert GetRenderFrameParams(frameToFreeInstancesFor)->m_gfxEopTick in FrameBegin, fg-draw-mgr.cpp:234; 14:17 and 15:48, both with the draw pipe on; none in about ten sessions with it off):** FrameBegin (U2 1.02 0xbd4260) takes frame N-3's params slot (0x1588630 + (N & 31) * 0xa70) and asserts its m_gfxEopTick (+0xa28) is set; PrepareNewRenderFrame (0xb62e00) zeroes it and s_lastPreparedFrameNumber is 0x10508d0. m_gfxEopTick is the GPU clock an end-of-pipe event writes. With the draw pipe that event is deferred to the recording thread (SignalAfterHostCopies); WriteData, EOS and compute releases wait for deferred signals first, but RunDmaData did not, and its CPU paths (memcpy, and since this morning WriteThrough for fills/copies into GPU-written pages) wrote guest memory at once, so a DMA-written frame marker could overtake the timestamp. Fix: RunDmaData into memory waits for host copies and deferred signals first (in test).
- **MT crash fix held** (11+ minutes with the draw pipe, 15:53 session). **MT frame time:** the wait profile showed buffer_cache.cpp:846 (CompleteReadbacks wait) about 1.5 s per 5 s: every ray-cast readback (about 36/s) had a guest write into its 1 KiB range while pending ("CPU writes that waited for one" = readbacks), about 8 ms each, a third of the frame. Only with the draw pipe (non-MT sessions: 0 waits): the recording thread runs behind, so the guest refills the result buffer for its next batch before the readback is submitted. Fix: EOP readbacks are yield_to_cpu: a CPU write into a piece adds it to `rewritten` and drops it from gpu_modified_ranges (no wait, no download); counted as "ray-cast pieces dropped for CPU writes" (in test).
- Live toggle A/B in MT (cost per draw on the recording thread): PreparedResources/DrawPreparation (4096/8192) 3.3 -> 3.2 us/draw, at most +1 FPS; DeferredStreamCopies/ParallelCopies (16384/131072) about +1-2 FPS in non-MT (noise). The default mask 159760 stays.
- **Frames queued 1 -> 2 (U2, 17:4x):** the draw recording thread spent 41% in ZwWaitForMultipleObjects and vk_presenter.cpp:609 waited 2.2 s per 5 s (CPU and GPU took turns; GPU 50-70%). With 2 frames queued the present waits went to 0 and the heavy spot rose from 33-37 to 42-50 FPS (MT), but the FrameBegin m_gfxEopTick assert came back after about 6 minutes (MT + 2 queued, 17:48, log u2-mt-q2-crash-1748). MT + 1 queued ran 24 minutes after the DMA fix. With threaded Vulkan recording off (mask bit 16, the default) EOP signals are not deferred to the copy threads, and the EOP flip runs in order on the recording thread (SignalFlip waits for deferred signals), so the remaining MT race is not yet understood. Next: non-MT + 2 queued (no crash seen without MT).


## 16 October 2026: readbacks, ray-cast fences, floating NPCs (0.1.2)

- **U3 Cartagena crash / flying characters:** async readbacks unprotected (GOW3_ASYNC_READBACK unset =
  2) let late readbacks overwrite fresh guest data (0xffff... pointers, compute ring with a garbage
  IB). U2/U3 now use mode 1 (probe_uncharted.py). Mode 1 hung U3 loading: the GPU thread faulted on a
  read-protected page inside MemoryTracker::ForEachUploadRange (region spinlock held) and the handler
  took the same lock -> uploads copy through the backing view (runtime_memory_copy_backing).
- **Mode 1 cost:** 89% of read faults were false sharing (BufferCache::noisy_pages: unprotect for
  10 s), the rest the 16-byte luminance (cs 7095d6c2/4f81a8f) read every frame -> recurring pieces
  (same address and size landed < 500 ms ago) stay readable. U2 33-46 -> 60 FPS with mode 1.
- **U2 NPCs from the sky:** EOP labels written at record time; the guest read ray-cast results
  (cs c92150ba) before the GPU ran them, and the 15 Oct drop of pending ray-cast pieces on CPU
  writes discarded ~250 of ~250 per 5 s. Now an EOP that carries ray-cast readbacks writes its
  label/interrupt when the readback lands (BufferCache::DeferUntilLastReadback); while one is
  pending all later EOPs queue behind it (strict order: a later fence first let U2 overwrite a DCB
  the GPU thread was reading, "Unimplemented PM4 type 0"); the command buffer is flushed at once;
  readbacks are polled every 64 packets and while idle; WAIT_REG_MEM counts a deferred value as
  written (landing it instead cost 18 FPS). Without the flush the readback was never submitted ->
  FrameBegin m_gfxEopTick assert (the ray-cast EOP is the frame EOP in U2). Fences land in 2-9 ms.
- **Floating Nate/NPCs (U2), confirmed by the user in play:** 2 frames queued, DeferredStreamCopies
  (toggle 16384) and a hot-window cooldown each brought it back (they delay or drop GPU data the
  game reads, or copy guest data later). Defaults: 1 frame queued (launcher config version 6),
  mask 159760, no cooldown (GOW3_HOT_WINDOW_TTL=<s> for tests).
- **Performance:** runtime_memory_copy/read/write_backing use the per-thread region cache (the
  winpthreads rwlock per call was 10-15% of the GPU thread): heavy U2 7.9 -> 6.4 us/draw. GPU
  checkpoints only with UNCHARTED_GPU_CHECKPOINTS=1 (~2%). Profiler call stacks (GOW3_PROFILE,
  profile_*_stacks.txt). Remaining in heavy U2 (3,000 draws, ~50 FPS): driver ~19%, per-draw stream
  copies ~9% (must stay inline, see above), image allocation churn ~2.6%, readback volume ~2 GB/5 s.

## 16 October 2026 (night): U2 floating NPCs / sinking Nate / leg flicker - real cause and fix

- The earlier settings "fixes" (frames queued 1, mask 159760, hot-window cooldown) were
  coincidences: 0.1.1 rebuilt from its tag floats too, and so does another port (Supermedo #33).
- Ruled out by user tests: MXCSR/x87 defaults (FTZ+DAZ 0x9fc0 / 0x037f on every guest thread),
  exact synchronous ray casts (GOW3_EOP_READBACK=2), both scalar-copy HLEs on the GPU
  (GOW3_SCALAR_COPY_GPU=all), the old upload path, GDS-to-memory counter copies (a per-frame
  statistic in a 7-slot ring at 0x1037521bf4).
- Precise readbacks (GOW3_READBACKS=2) fixed it (3 user checks) at ~50 FPS: the guest polls fence
  labels (guest code 0xc1e20a / 0xc1e5bd) on a page that also holds GPU data, 60k read faults/s.
- GOW3_FAULT_STATS now lists, per 1 MiB window, the CPU reads that hit GPU-written bytes (with
  addresses) and the shaders that wrote the window. In precise mode the guest read only the
  luminance value and one value (e.g. 0x1062820014) in 1 KiB blocks written every frame by copy
  shader 6a6422a9. Read back with the submission they counted as recurring pieces (readable, a
  frame old).
- Fix (GOW3_COPY64_EOP_READBACK=1, set for Uncharted 2 in probe_uncharted.py): 6a6422a9 outputs
  join the end-of-pipe readbacks (Rasterizer::eop_readbacks; the fence waits for them,
  RunEventWriteEop). Only the dwords the dispatch writes (min(groups * 64, s6, num_records)) and
  only copies up to 64 KiB: its two large copies per frame (~0.2 and ~2.4 MB) were +300 MB/s of
  readback and are never read. User: NPCs and Nate on the floor, no leg flicker (the flicker came
  back in a run with the fix off), 60 FPS. Readback volume ~2.95 GB/5 s, as before the fix.
- Next for low-end PCs: the pre-existing ~600 MB/s of readbacks (hot windows).


## 16 October 2026 (afternoon/evening): MT fixes, readback cut, optimizations on, U2 aim focus, GPU hang (0.1.2)

- **Multithreaded rendering:** U3 m_gfxEopTick assert = flips queued on the GPU command thread
  overtook the end-of-pipe fences the recording thread writes later (driver.cpp QueueFlipInOrder
  through RunInOrder); then a crash in SyncSrtTables = boost::icl RangeSet gpu_modified_ranges read by
  stage A while stage B changed it (shared_mutex, GpuRangesAdd/Subtract). U3 MT 8 min clean. U2
  floating came back with MT: the stage-B fence path skipped DeferUntilLastReadback; fences with
  readbacks now take the waiting path there too, flips likewise; stage A drains before touching
  readback state; deferred_signals atomic (user-confirmed).
- **Readback volume** (GOW3_READBACK_STATS=1): nearly all was 1 MiB windows of the effect shaders
  (cs 8f067540, 7d7e7206, 0c936dd6), which the CPU never reads (precise-readback statistics). Hot
  windows cool after 3 s (GOW3_HOT_WINDOW_TTL), small writes read back in 256-byte granules
  (GOW3_SMALL_READBACK_ALIGN): U2 2.9 -> 0.46 GB/5 s, U3 3.1 -> 0.48, U1 0.63 -> 0.16 (user-confirmed U2).
- **Disabled-optimizations default 159760 -> 0** (tools/defaults): live A/B at one spot, heaviest U2
  view 43.8 -> 49.0 FPS; U3 GPU command thread idle 22 -> 29%. Played for hours by the user.
- **U2 aim blur:** the aiming depth of field (debug "Enable Aiming Dof", on) reads its focus distance
  from a 1x1 R32F render target (guest code 0x44e2cb reads *(u32*)ptr << 8): never copied back, 0.
  Chain: ps 9f801363 (32x32 crop of the depth at 8 CPU constants) -> 0a15b09b (16x16) -> 784b6f74
  (1x1 min). TextureCache::ProcessTinyReadbacks copies 1-row images into the buffer cache and the
  range comes back with the next EOP fence, unprotected (GOW3_TINY_IMAGE_READBACK=1, U2 only).
  First versions crashed (m_gfxEopTick): protected pages made game threads wait for the GPU; then
  whole pitch rows of staging garbage; then a landing after the fence onto reused memory. Above
  1080p the crop centre c[0],c[1] = HUD crosshair (1920x1080 space) / render size: 0.25 at 4K;
  FixAimFocusCentre divides by 1920*c[6], 1080*c[7] (GOW3_AIM_FOCUS_FIX=0 off). 4K/1440p/1080p
  verified (GOW3_TRACE_PS dumps render targets, viewport, textures and constants).
- **U2 GPU hang after 90 min** (game: "Device lost during submit" after "GPU wait: 1 seconds"; OBS:
  DXGI_ERROR_DEVICE_HUNG the same second): cs 8f067540 bound a garbage V# in mapped memory
  (unaligned, stride ~15 KB, ~1e9 records). For 8f067540/7d7e7206 an unaligned base, stride >= 8192
  or size > 64 MiB is garbage (real bindings <= 33 KB, aligned).
- **Memory/thread growth with OBS:** threads starting in nvwgf2umx.dll (NVIDIA D3D driver) grew ~6/s
  (~90 MB/s) only with OBS's implicit Vulkan layer (obs-vulkan64.json; DISABLE_VULKAN_OBS_CAPTURE=1
  stops it) while OBS kept re-hooking ("failed to open shared handle"); NVIDIA presents Vulkan
  through DXGI/D3D12 here, so OBS hooks both. Not a port leak; README/release notes give the
  NVIDIA "Prefer native" present method or Window Capture.
- Profiler: RtlLookupFunctionEntry deadlocked with the target suspended and RtlVirtualUnwind read
  garbage frames; now its own .pdata lookup and unwinder with checked reads.
- Frame rate limits 40-55 (overlay and launcher) for the U3 cloth flicker tests (60 FPS only).
- GitHub: #1 (U3 crash) and #4 (U1 crash after the boat scene, v0.1.1; user-tested fixed) answered
  and closed.


## 17 October 2026 (night): U3 Sully cloth flicker FIXED (my tests, not yet user-confirmed)

- **Cause:** cloth solver cs 1c957b01 (1024-thread groups, 2 KiB LDS) does warp-synchronous LDS
  reductions (no s_barrier between steps inside one 64-lane wave). shadPS4 only inserts LDS barriers
  for groups of exactly 64 threads; on 32-lane subgroups (NVIDIA) the wave's halves race. The game
  picks the cloth sub-step count from dt (cb#1 dword 160: 2 at 40-60 FPS, 3 at 30): with 2, every
  dispatch with sane input exploded (alternating frames). Proof: AMD iGPU (GOW3_GPU_ID=1, wave64,
  GOW3_TIME_SCALE=3 to get 2 sub-steps at 15 FPS) never exploded.
- **Fix (shared_memory_barrier_pass.cpp):** MultiwaveLdsBarriers(hash) (default 1c957b01;
  GOW3_LDS_BARRIERS_MULTIWAVE=0/1/hex list) runs the barrier insertion for multi-wave groups on
  32-lane GPUs with a stricter uniformity test (IsGroupNonUniform), skips shaders with an early
  Return, and only puts merge-block barriers after ifs/loops that access LDS. Without that last
  rule NVIDIA hung (m_gfxEopTick) on a barrier in a loop without LDS ops that the dispatches never
  entered (found by bisecting with GOW3_LDS_BARRIER_SITES). Cached SPIR-V of listed shaders is
  named <hash>_<perm>_lds1, so old caches recompile only them.
- **Result:** 0 of ~1200 dispatches exploded (was ~50%), 0 flicker frames in 40 shots, 60 FPS, with
  the user's shader cache. GOW3_LDS_BARRIERS_MULTIWAVE=1 (all 12 U3 LDS shaders, 84 sites) also ran
  clean, not adopted (U1/U2 untested). U2 60 s check unchanged (60 FPS, no errors).
- Diagnostics added: GOW3_LOG_LDS, GOW3_LDS_BARRIER_SITES, GOW3_CLOTH_PEEK, GOW3_TIME_SCALE (all
  guest clocks), GOW3_DUMP_CS_CONST.

## 17 October 2026 (night, 02:30-04:40): low-end PCs

- **Weak-PC simulation:** AMD iGPU (GOW3_GPU_ID=1, about a quarter of a PS4 GPU), 720p, game
  process affinity 0x55 (4 cores). Per-thread CPU with names: runtime now sets Windows thread
  descriptions for guest threads; scratchpad threadcpu.ps1 (OpenThread 0x40 + GetThreadDescription).
- **Found:** the six NdJobWorkerThreads spin with `pause` when idle (U3 worker loop 0xda6470): the
  job manager's sleep flag (byte +0x59, SetSleepMode 0xd96440, set on by the game once, off in four
  places) is off. 4 cores: 390% of 400% CPU, GPU thread 5%; U3 title 19-27 FPS on the RTX 5080,
  level 5-7 FPS on the iGPU.
- **Fix:** tools/probe_uncharted.py JOB_SLEEP_PATCHES (3 pattern patches, same bytes in U1/U2/U3
  1.02): the worker loop, its re-check before waiting and the submit path treat the flag as on, so
  an idle worker waits on the manager's condvar after 1000 spins (10 ms timeout) and every submit
  broadcasts. UNCHARTED_JOB_SLEEP=1/0, default auto = laptops (GetSystemPowerStatus battery) or
  <= 12 logical CPUs; launcher Performance -> Idle game threads (Auto/Sleep/Spin).
  UNCHARTED_JOB_SLEEP_SPINS=<n> (diagnostics) changes the 1000.
- **Results:** 4 cores: U3/U1 title and level 60 FPS, 1-1.5 cores used; iGPU + 4 cores U3 level
  27.5 FPS (5-7 before; now GPU-bound, the iGPU is ~4x slower than a PS4). 32 threads, Sleep: U2
  heavy spot uncapped 76.5 -> 78.2 FPS. MT rendering on 4 fast cores: 72-75 off vs 69-72 on.
- **Garbage resource tables:** 1 of 11 four-core Sleep runs stopped with "Thread ID buffer
  addressing is not supported outside of compute" after SRT walker faults at -1 pointers; also
  seen with GOW3_FINISH_AT_FLIP=1 (reproduces it about every second run: VS d5cdb3af walked a
  table of -1 pointers, a draw of 38d69960 compiled from a garbage V#) and GOW3_SYNC_GPU_CONSTANTS=0.
  The GPU thread's own walk reads it, so the table changed before that thread got there; the game's
  frame recycling (FrameBegin N-3, PrepareNewRenderFrame N-32 m_gfxEopTick checks) is frame-based
  and fine. Root cause open. Now tolerated: resource_patching_pass ignores add_tid outside compute
  (logs the shader); the walker fault handler skips the faulting load (register 0) the first 16
  times per instruction instead of patching it to 0 for good (logs "SRT walk fault: shader ...
  user data ..."). With FINISH_AT_FLIP the game kept running at 57 FPS through them.
- **GPU thread:** Rasterizer::IsMapped keeps 4 per-thread mapped intervals (generation bumped on
  unmap): SyncSrtTables 12.1% -> 6.5% of the GPU thread, idle 14.8% -> 19.9% (U3 street, 60 FPS).
- **iGPU limits left:** GPU thread waits ~1.8 s per 5 s in synchronous drains (DownloadMemory
  scheduler.Finish, CompleteReadbacks), ray-cast fences 30 ms late, ~27k write faults/5 s in the
  cloth window 0x1110100000, treasure cs 8f067540 dispatched 422x/frame (1x1x1 groups).
- **Garbage tables, more data (04:15):** a fault report (Rasterizer::ReportSrtFault, runs after a
  walk that faulted: every table of the walk, now and 3 ms later) with 4 cores + Sleep +
  GOW3_FINISH_AT_FLIP=1: VS d701fef2 / e467c702 user data point at 0x1027008f40 / 0x1027008fc0,
  which hold floats (constants), not the expected pointer table; unchanged 3 ms later. So the
  draw's user-data pointer refers to per-frame memory the game had already reused when the lagging
  GPU thread got to the draw (not a table being written during the walk). CE/DE counters are
  stricter than hardware (the CE waits whenever it is ahead), so not a constant-RAM ring overrun.
  Next: which fence lets the game recycle 0x1027xxxxxx early (compute-queue EOPs processed ahead
  of graphics? deferred labels?).
- **Job sleep default (04:30):** on for every PC (UNCHARTED_JOB_SLEEP=0 / launcher Spin turns it
  off). 3 x 2 min U3 on all 32 threads with Sleep: no walker faults, 60 FPS, 2.2-2.6 cores in all
  (6.4 with Spin, U1).
- **GPU_WAIT_PATCH loop (U3 0xe5f610):** submits each entry's command buffers and, only when a
  flag byte (+0x34 of a global at 0x18c6280-ish) is set, spins until the entry's label (+0x20)
  equals the expected value with the 2 s timeout we NOP. Not the per-frame wait; FrameBegin's
  m_gfxEopTick check relies on something else. Open lead for the early-recycling question.

## 17 October 2026 (day): cloth fix for both solvers, 4K CPU work, MT compute fences

- **U3 Sully shirt (third step):** the 128-thread solver cs 809864d3 has the same convergence loop
  as 1c957b01 (exit condition read from lds[0], rewritten at the top of the next pass; 17 barriers
  in the loop without the barrier-region rule, 47 in 1c957b01 with it). BarrierRegionShader default
  now {1c957b01, 809864d3}, cache suffix _lds4 (vk_pipeline_serialization.cpp). The earlier
  "809864d3 hangs with the rule" came from a GOW3_DUMP_SHADERS run that stalled before 809864d3 was
  even compiled. With the rule: 29 sites, no hang standing or walking. Diagnostics:
  GOW3_CLOTH_TRACE=1 (who reads the solvers' outputs, missed-barrier check; no GPU copies, unlike
  GOW3_CLOTH_PEEK, whose copies add sync points), "Cloth outlier"/"Cloth jumps" lines (the cloth is
  body-local, |x| ~2, so the old >1e4 test never fired). Chain: solver s6 + previous s29 -> skinning
  cs 7d7e7206 -> draws; every solver -> 7d7e7206 read had a barrier.
- **Static ranges (buffer_cache.cpp UseArenaForStatic):** U3's crowd skinning cs 8f067540 (~1,470
  dispatches/frame) reads 5 static mesh arrays per character (1-8 KB); the stream path (<= 16 KB,
  read-only) re-copied them on every bind: ~790k copies/s, 12 GB/5 s (GOW3_STREAM_STATS=1). A range
  >= 1 KiB bound in 3 submissions now lives in the arena (CPU write tracking; demoted for good once
  CPU-modified); clean ranges take a fast path (GetArena only); tracked in 64 KiB pieces in the
  barrier tracker. Never where the runtime writes through the backing view (labels, readback
  landings, image downloads: MemoryManager::BackingWritten bitmap), where GPU data may share the
  pages (MayBeGpuWritten), or in kept-writable fiber stacks. GOW3_STATIC_BUFFERS=0 off.
- **Compute RELEASE_MEM fences:** the plain store faulted on pages with GPU data and the fault
  fetched old GPU bytes on the second queue, waiting for the GPU (~1.7 ms, ~40/s, 6% of U3's GPU
  thread; found with GOW3_FAULT_STATS write RIPs). Now through TryWriteBacking + NoteLabelWrite when
  MayBeGpuWritten(page) (PM4CmdReleaseMem::SignalFence(irq, gds, write_mem)); unconditional
  bookkeeping cost U2 (~45k releases/s) 4%.
- **Other GPU-thread cuts:** compute dispatches merge overlapping/adjacent bound ranges before the
  barrier tracker (ResetBindings; draws keep the old path, the sort cost U1/U2 3-4%); lock-free
  64 KiB GPU-window bitmap before the GPU-written range set's lock (SRT walk); equeue notify only
  when an event becomes pending and a thread waits (m_waiters).
- **Multithreaded rendering:** RunComputeReleaseMem on stage B defers the fence through
  SignalAfterHostCopies like end-of-pipe fences (NoteLabelWrite on stage B; the callback writes the
  value and raises the interrupt).
- **Results, 4K, fps_limit 120 (RTX 5080 / 9950X3D):** U3 checkpoint 63.9 -> 79, crowd 68.9 -> 90;
  U2 museum spot 84 -> 99; U1 94.5 -> 101. MT: U3 100.5, U2 118, U1 117. 4 cores, 1080p: U3 ST 90,
  MT 120 (cap). 5-minute U3 soaks at 1440p/60 (Spin), ST and MT: no errors.
- **Rejected:** same-submission stream copy reuse (12% hits, slower; GOW3_STREAM_REUSE=1),
  splitting the copy batch at fences (GOW3_COPY_SPLIT_KB), GpuRangesAdd memo (no gain).
- **Open:** U3 on the AMD iGPU (GOW3_GPU_ID=1) stops flipping 0-35 s into the level; also with the
  04:13 build. Not: static ranges, equeue, host arenas, COPY64, job sleep, async shaders,
  frames_queued 2. U1/U2 fine on the iGPU. lldb (scratchpad hangcatch.sh): GPU thread idle, no
  deferred signals, main thread polling a job counter (guest 0xd975b0, from 0x566c89), workers
  idle, frame submission thread idle (cond wait at guest 0xe5fb6b).

## 17 October 2026 (evening): raw mouse look (U2/U3) and keyboard/mouse button icons (all games)

- **Mouse look** (src/runtime_mouselook.c, installed by probe.c `install_mouse_look`): the entry
  of each game's camera input function is patched with a jump to a stub that calls a handler with
  (camera, look direction, stick). U3 0x4e42f0, U2 0x428f40 (image offsets, 1.02): yaw rotates the
  look vector about +Y, pitch goes through the game's own step function (U3 0x100ef90 on
  camera+0x7750, pitch at +0x7874; U2 0x4288d0, pitch at +0x9e60), scaled so a count turns the
  same angle both ways. 0.0011 rad/count x GOW3_MOUSE_SENS. U1 not done: its camera reads the stick
  through 0xb7430 (input at params+0x10, right X +8, Y +0xc); caller 0xb62a5 (vtable+0x98).
- **Button icons** (gpu/shim/gow3_prompts.cpp): all three games draw the button icons in their
  text from one symbol font, data/fonts/ps4/symbol3.fnt (BMFont text, 32 glyphs, 512x512 atlas;
  text tags like |X| |T| |move| map to glyphs; 1-30 and 40-49 are two drawings of the buttons).
  The atlas is a CPU-written R8G8B8A8 1D-tiled image (tile mode 13: 8x8 tiles in rows, Z order
  inside, verified against a texture-cache dump). U1/U2: one RGBA texture; U3: colour (alpha 0)
  and a separate alpha texture (grey in RGB). Recognised in TextureCache::RefreshImage by the XXH3
  of the guest data (U1 ad5ada34fd34c76b, U2 97779d0906f16551, U3 8d03f2ff457f032d +
  ae0c06306763d424); while keyboard/mouse icons are on, a redrawn linear atlas uploads instead
  (key caps from stb_truetype SDFs of the embedded DejaVu Sans, mice as analytic SDFs; cells not
  redrawn, e.g. Share, keep the original). runtime_pad.c publishes the labels from the live
  bindings (L2/R2 prefer the mouse) and the device used last (pad buttons, sticks past half way,
  triggers vs mapped keys, mouse buttons/movement); a switch calls TextureCache::InvalidateMemory
  on the atlas, which re-uploads at its next use. GOW3_PROMPTS=auto|keyboard|controller (launcher
  Controls -> Button icons), GOW3_PROMPTS_LOG=1 logs every 512x512 upload's hash.
- Test aids: GOW3_PROMPTS_LABELS=1 (labels with keyboard/mouse off), pad-file tokens kbm / pad
  (device use), GOW3_WINDOW_BACKGROUND=1 (window opens unfocused, behind other windows, never
  raised), the presenter dump trigger's `size W H` line (every cached image of that size).
- **In-world button pictures** (the Triangle beside a chest): separate BC1 pictures, 256x256 with
  9 mips, 1D-tiled, stored upside down, colour textures masked by shared "trans" alpha textures
  (hard circle, soft glow). Found with a texture-cache dump in the user's U3 session (presenter
  trigger `size 256 256`), then all of them by scanning the paks for 1D-tiled BC1 images with a
  uniform border (scratchpad pakscan.py): U3 hud.pak face buttons + L1/L2/R1/R2, U2 hud.pak and U1
  pak19/particle.pak the same four face buttons (identical in U1 and U2). The game's data equals
  the pak's for mip 0; the padded tiny mips differ (not deterministic), so they are recognised by
  the XXH3 of mip 0. gow3_prompts.cpp decodes mip 0, flips it upright, keeps the button and glow,
  paints over the symbol (face: inner disc; shoulder: the text box clipped to the button) and
  draws the binding in the symbol's colour (a mouse for mouse buttons), re-encodes every mip to
  BC1 (principal-axis endpoints, punch-through kept) and uploads linear mips. Offline check:
  scratchpad prompts-test.exe (gow3_prompts.cpp built with GOW3_PROMPTS_TEST) + runpics.sh.
- **Controller icons:** label set 1 for non-PlayStation controllers (SDL_GetGamepadType; letters
  from SDL_GetGamepadButtonLabel, so Nintendo pads get theirs): A/B/X/Y in Xbox colours (pictures
  recoloured), LB/RB, LT/RT, LS/RS, Menu, View; PlayStation pads keep the game's icons. Device 0/1/2
  = PlayStation / keyboard / drawn controller; GOW3_PROMPTS adds playstation and xbox.
- **U1 mouse look** (0xb7430 orbit camera, stick-equivalent injection) is behind
  GOW3_MOUSE_LOOK_U1=1 until tested; GOW3_MOUSE_LOOK_DEBUG=1 logs its inputs once a second.
- **Not yet verified in game (17 Oct 21:47):** the in-world pictures and the controller icons
  (only the offline redraw); the text icons are verified in U1/U2/U3. Not done: U2's shoulder
  pictures (not found by the scan), the stick and d-pad pictures.
- **Corrected picture hashes + HOLD pictures (17 Oct 23:37):**
  - The pak scan's 32-byte aligned start offsets were up to 24 bytes late, so the first U1/U2 face hashes were wrong. The scan now walks back to the first border block.
  - Triangle, Circle and Square are the same in all three games (5db884aa…, 7af1abf6…, 9edd4f2f…). Cross differs: U1/U2 2bf5d90e…, U3 305c2c73….
  - The flat "HOLD" buttons are 512x512 BC1 pictures with two press positions. They were found live in the user's U2 session with the new dump-trigger lines `max W H` and `range LO HI` (up to 256 images, any format), and they match U2 hud.pak 0x828c98.
  - The HOLD Triangles are now swapped: U1/U2 25d954ce… and c96e34fb…, U3 4181f3ae… and 07ff3d0e….
  - The redraw covers the symbol out to its farthest saturated texel, so the old symbol's tip no longer shows.
  - Still the game's own: the U3 HOLD Circle/Square (decode garbled), the U1 shoulder HOLD pictures, and the U2 HINT d-pad picture.

## 18 October 2026 (night): FrameBegin m_gfxEopTick crash, aim sliders, in-game controls

- **m_gfxEopTick (FrameBegin, fg-draw-mgr.cpp:234):** most reported crash (GitHub #11 is another
  port's log; #12 x2 U1 RTX 3050/5060, #17 U3 RTX 3050, the user's U2 17 Oct 21:52). Logs show
  40-55 ms stalls just before (compiles, GPU waits). The timestamp is not an EOP label: the game's
  GPU event thread (ProcessGpuEvent, U2 0xc1e080, store at 0xc1e4cc) sets it on the end-of-pipe
  interrupt after checking the frame marker (Data64 EOP, m_frameWriteBack). Not reproduced on the
  desktop: held/lagged readbacks (GOW3_TEST_READBACK_HOLD/LAG), command thread stalls
  (GOW3_TEST_CP_STALL), late timestamps or interrupts (GOW3_TEST_LATE_TIMESTAMP/IRQ) only slowed
  the game (it waits); delaying 64-bit EOP data alone asserted m_frameWriteBack instead (test
  artifact). Overtaking signals while a fence waits for its readback: one WRITE_DATA and one
  flip interrupt per frame (GOW3_TEST_OVERTAKE_LOG). Fix: FRAME_BEGIN_WAIT_PATCHES (probe_uncharted.py,
  all three games): the assert's 59-byte path becomes a pause loop until the timestamp is set (up
  to 50M iterations), then continues. U2 with it: 59.6 FPS, no change in normal play.
- Aim sliders: AIM_RANGE_PATCHES U1/U2 5x, U3 4x (bench-tested by poking the value and stepping
  the slider). In-game Controls tab and controls in the settings file (see memory
  project-ingame-controls).

- **Hitch analysis (18 Oct night):** GOW3_PROFILE=<big> GOW3_PROFILE_STALLS=1 (and
  GOW3_PROFILE_GUEST=NdJobWorkerThread) writes the stacks every profiled thread had during each
  frame over 40 ms (dumps/stall_<n>_<thread>_stacks.txt). "Slow submit" (vkQueueSubmit > 8 ms),
  "Slow shader compile" / "Slow shader wait" (> 8 ms on the GPU thread) are logged. Findings: the
  game's job workers idle in hitches, the GPU command thread is the bottleneck: (1) compute
  shaders translated on the GPU thread at first use because NVIDIA (pipeline binaries) used the
  on-demand start without warm-up - fixed, background warm-up is the default now; (2) one
  vkQueueSubmit held 70 ms in the NVIDIA driver (kernel calls) in U2; (3) streaming frames upload
  ~115 MB of new images in one frame (resident/protect/image create ~16 ms). The user's 12.7 min U2
  session: 12 hitches over 40 ms, 1 with compiles.
- **Menu shortcut (GitHub #15):** Gow3Settings menu_shortcut (lt_r3 / view_menu / keyboard), in the
  Controls tab; the menu title shows the current one.

- **Draw distances (U2/U3):** debug-menu variables (dmenuvar2.py: item +0x38 = var, often
  `mov eax, OFF; add rax, [slot]`): render settings U2 0x1247b40 / U3 0x18c6280 (+0x150 temporal
  fade scale, +0x168 cutscene fade scale, +0x180 "Fade Distance Scale", +0x844 cascade fadeout),
  light settings U2 0x1162820 / U3 0x1b3c130 (+0xc0 max sun shadow dist, +0xc8 point/spot, cascade
  override and 4 distances at +0x218.. U2 / +0x190.. U3). Defaults 0.58 / 0.5 / 0.58, 40 m, 40 m.
  src/runtime_gamesettings.c recognises them by those defaults and writes the menu's percentages
  twice a second (Image tab, fade_distance / sun_shadow_distance / light_shadow_distance). Visible
  effect not confirmed: the bench saves (U2 shop interior, U3 chapter 2 rooftops) show no change in
  draws or screenshots; the user is to try them where pop-in shows.

- **Ultrawide:** the render resolution patch (resolution_patch, display settings sizes) at a 21:9 or
  32:9 size is all it takes: the games compute the projection from it (wider view, Hor+), the HUD
  and pause menus lay out fine, the aim focus fix maps the crosshair box (0.3750 -> 0.5000).
  Tested U1/U2/U3 at 2560x1080 and U1/U2 at 3440x1440 (60 FPS, no asserts). Launcher: Aspect ratio
  (ASPECTS, aspect_resolution); probe_uncharted.py steps ultrawide sizes down in the same shape.

- **Temporal anti-aliasing (18 Oct morning):** ported from reference/uncharted_pc_supermedo (GPL v2):
  GOW3_VELOCITY_PS=f4b18264 (the motion blur's camera velocity pass, 960x540, same in all three
  games) gives the camera: CameraMotion::OnVelocityConstants reads its constant buffer at the draw
  (z = c9 / (depth - c8), view = (z (c0 u + c2), z (c1 v + c3), z), rows c12..c23 to world, c40..c55
  previous clip x/y/w), camera_motion.comp mode 2 writes the motion vectors (pixels, previous minus
  current). Scene depth = the frame's first pass at GOW3_SCENE_SIZE (probe_uncharted.py passes the
  patched render size) that is depth-only or draws RGBA16F (U2 #0: D32S8 + RGBA16F 0x11478e0000).
  GOW3_UPSCALE_BEFORE_CS=aec539f0: AA runs on the lit scene before the post. Native AA only
  (preset 0, the upscaler's output stays its "native" 1920x1080 so Scaled() is false; the images'
  own sizes carry 1440p/ultrawide). taa.comp's depth test gets the velocity camera too
  (TaaDepthParameters: proj = (2/c0, -2/c1, c8, c9), previous view z = previous clip w row; the
  reference left it zero, which rejects all history). Near = |c9/c8| (U2 0.24), vertical FOV =
  2 atan(|c1|/2). The native path now dumps too (GOW3_DUMP_TRIGGER: input/motion/output/depth).
  Measured (8-frame dumps, scratchpad taatest.sh + taaeval.py): top-5% flicker pixels U2 0.043 ->
  0.019 (TAA) / 0.018 (FSR 3.1), U3 0.137 -> 0.050, U1 0.071 -> 0.038, U3 3440x1440 0.103 -> 0.042;
  turning: motion +51 px/frame for a right turn, near zero on Nate (orbit pivot), no trails. RTX
  5080 U2 4K: 120 FPS cap with TAA/FSR 3.1/off. Menu: Image -> Anti-aliasing (temporal_aa_available
  set by the renderer); launcher Display -> Anti-aliasing (ini "upscaler", default off; probe adds
  upscaler=off to a settings file without it, the renderer's own default being FSR 3).
  FSR 4 needs gow3_pc/fsr4_shaders (native model; listed only when present). DLAA needs
  gow3_dlss.dll (gpu/dlss_bridge, MSVC + NVIDIA DLSS SDK) and nvngx_dlss.dll beside Uncharted.exe.
- **DLAA and FSR 4 (18 Oct, 08:00, downloads approved by the user):** tools/fetch_aa_files.sh
  fetches the DLSS SDK v310.9.16 files (headers, nvsdk_ngx_s.lib, nvngx_dlss.dll: Authenticode
  valid, NVIDIA Corporation) into downloads/ and builds the bridge with MSVC (Build Tools 2022, CMake
  "Visual Studio 17 2022"; Vulkan headers copied alone from CLANG64), and the FSR 4 v07 INT8 native
  model (1080/2160 tiers) into gow3_pc/fsr4_shaders. package_windows.sh ships them with
  licenses/nvidia-dlss, gow3-dlss-bridge and fsr4; the workflow runs the script (continue-on-error).
  Measured: DLAA U2 top-5% flicker 0.038 -> 0.013, no trails turning; FSR 4 U3 0.119 -> 0.050,
  colours unchanged. RTX 5080 U2 4K: DLAA 84 FPS vs 120 off (GPU thread busy 99%: the model's
  GPU time, preset 0 = NGX default).

- **Cutscenes fast/out of sync above 60 FPS (user, U3, 18 Oct 08:30):** the movie player shows a
  frame per vblank; GOW3_VBLANK_HZ=0 (480 Hz) with limit 120 sped it up. Fix after the reference
  port: shim/videodec2 stamps Libraries::Videodec2::last_movie_frame_ns on each sceVideodec2Decode;
  PresentThread (videoout/driver.cpp) gives 60 Hz vblanks and flips while a frame was decoded in
  the last 300 ms (log "VideoOut: movie playing, 60 Hz vblank" / "movie ended"). The user's U3
  session showed 64-75 FPS at ~350 draws/frame with the 480 Hz vblank. Built 08:36, awaiting the
  user's test (they were playing; not bench-tested).

- **U3 rooftop freeze (18 Oct, FIXED):** hangprobe.py (thread RIP histogram + user/kernel time +
  register target protection) caught a guest thread 2.2-2.6 s of 3 in the kernel on `andq $-4,
  0x28(%rbx)` (e5dfbd, a fresh 0xf2a0 ray-cast batch at 0x1023900000, page NOACCESS): a write
  into a page a pending buffer readback read-protects (PageState::Perms: read watchers block
  writes) found no GPU bytes under it and returned; nobody landed the (finished) readback
  because only the GPU thread polls and it was idle waiting for the game (SubmitDone) -> fault
  loop; other workers spun on the ray-cast lock (ea5fc0). Fix: page_manager.cpp counts
  same-page faults per thread; 4 in a row on a read-watched page -> Rasterizer::ReleaseStuckPage
  -> BufferCache::ReleaseStuckPage (on the GPU thread via SendCommand): PollReadbacksNow, else
  CompleteReadbacks(true, newest) (submitted), else UnwatchPage. Logs "Stuck fault N: ...".
  Rooftops: no freeze in 10+8 min (froze in 0.5-5 min before); ~32 events/s, nearly all
  "waited for the GPU". Presenter frames-ahead wait now polls readbacks every 0.25 ms
  (Semaphore::WaitFor; GOW3_POLL_WHILE_WAITING=0 off). GOW3_OPEN_RAYCAST_READBACKS=1 (opt-in):
  ray-cast EOP results without page protection (no pacing gain measured).
- **Frame pacing (18 Oct):** GOW3_FRAME_LOG=<file> logs guest flip submits (S) and presents (P);
  scratchpad pace.py gives FPS/median/p99/1%/0.1% lows. The "75.0 FPS" seen at every 120-limit
  setting was the user's 75 Hz monitor with VSync (FIFO): frames queued behind VSync and the
  guest was held back in bursts (2-3 frames of 6 ms, one of 25-35 ms). Frames queued 2 and
  multithreaded rendering did not change it (the GPU thread was 22-25% idle with 2). Fix
  (videoout/driver.cpp PresentThread): in the unlocked mode (480 Hz vblank) with VSync and a
  limit >= the display refresh (30-120 Hz, Gow3DisplayRefreshRate exact), the vblank and the
  present slots follow the display; switching live. U3 rooftops and U2 museum at 120+VSync on
  75 Hz: 75 FPS, present p99 14.8 ms, 1% low 66 (was 32 / 22), 0% over 20 ms. Limits 75/90/100
  added. Also measured: SubmitDone -> WaitGpuIdle serializes guest submission with the GPU
  thread (kept: the reference found heap corruption without it).


## 18 October 2026 (afternoon): unlock all chapters; #18; other copies of the game

- **Unlock all chapters (UNCHARTED_UNLOCK=1, launcher Advanced, off by default).** All three 1.02
  executables keep Bluepoint's tweak flags (debug menu items "Unlock Crushing/Brutal difficulty by
  default", "Unlock Chapter Select by default") in the struct behind U3 [0x1dd26a0] (+0x2c60/61/62),
  U2 (+0x2da0/a1/a2), U1 (+0x2c30/31/32; U1 reads them as one dword). The constructor sets Crushing
  1, Brutal 0, chapters 0, then copies Crushing and chapters from the *bp-ucctweaks* table. Patches
  (UNLOCK_PATCHES): chapters = 1 where the table is read, Brutal = 1 where it is cleared (U1 has two
  identical constructor copies, told apart by their call displacement). Verified on the user's saves
  (U1 at Plane-wrecked, U2 at 33%, U3 at ch. 3): Chapters menu reaches U1 22, U2 26, U3 22; Brutal listed in
  all three. 1.00: no match (option does nothing).
- **#18 (U3 device lost after the museum guard scene, RTX 5070 Ti):** both logs stop when young
  Sully streams in mid-play (sullivan-young, cloth cs 1c957b01, DMA fs 9eb7be58/4f83b1de/43573568/
  78f8ad3b); GPU stuck 2.2 s on one submission, then TDR. Same U3 executable as ours. My bench save
  (user's) loads at the checkpoint after it (anim-col-thrown-out), where Sully loads with the level:
  no hang on the 5080. Their first log loaded a checkpoint before it and hung within 10 s. Asked for
  their save + launcher-last-run.log.
- **Other copies:** 1.02 executables are the same across regions (#17's CUSA02826 U3 = ours: same
  size and relocation count); saves/caches go by TITLE_ID from param.sfo. Base 1.00 is the problem:
  only 4/8 U1, 2/10 U2, 2/9 U3 patches match (no m_gfxEopTick wait, NaN focus reset, job sleep,
  aim range, torch), and the mouse-look sites are 1.02 offsets with a byte check (off on 1.00).
  Dry-run checker: scratchpad patchmatch.py <uroot>...

## 18 October 2026 (evening): 0.2.2 fixes (#21, #22) and the GPU command thread profile

- **#22 (all three games at start, Windows 10 19041, i5-8600K):** FAST_FAIL_INVALID_SET_OF_CONTEXT
  in ntdll!RtlGuardRestoreContext after a handled fault at U1 0x4fc0b2 (`push rbx`) on a sceFiber
  stack in guest flexible memory (rsp 0x12002504a0), outside the TEB's stack. probe.c
  cover_current_stack(), first thing in vectored_fault: TEB StackLimit/StackBase name the
  allocation holding rsp (or the thread's own stack again). GoW3 has no fibers.
- **#21 (U3 Syria, 0.2.1):** PatchImageSharp asserted "FMask storage instructions are not
  supported" during a level load (garbage T#). Now a warning: the store is dropped (atomics give 0).
- **Launcher:** start and callback errors go to analysis/uncharted-user/launcher-error.log and a
  message box (pythonw has no console; a player's 0.2.1 launcher closed silently).
- **GPU allocation records** (image.cpp, for GPU fault reports) only with GOW3_GPU_CHECKPOINTS: they
  grew by every image and buffer made in a session and each free searched them.
- **Profile (U2, 1440p, RTX 5080, GOW3_PROFILE):** GPU command thread ~25% buffer bookkeeping
  (ObtainBuffer: SynchronizeMemory, GpuRangesAdd, protect), ~3% image frees in the kernel
  (D3DKMTDestroyAllocation2), ~7% interrupt delivery. On 4 cores (affinity 0x55) the game uses
  1.6 cores and its submissions wait 16-22% in WaitGpuIdle after sceGnmSubmitDone
  (GOW3_SUBMIT_WAIT_STATS=1; GOW3_SUBMIT_NO_WAIT=1 is a measurement only: +5% FPS, worst frame
  45 -> 18 ms, unsafe). Opt-in, no measured gain: GOW3_IMAGE_REAPER=1 (frees on a thread),
  GOW3_ASYNC_IRQ=1 (compute interrupts on a delivery thread). Multithreaded rendering on 4 cores:
  U2 108 -> 120 (cap), worst frame 50 -> 10 ms; U3 114.5 -> 112, U1 102 -> 103, pacing worse.

## 18 October 2026 (night): 0.2.3 (U1 weapon flicker, U2 focus NaN)

- **U1 weapons flat grey on some frames (user, GitHub #23):** only with multithreaded rendering (the
  user's A/B; my checkpoint run: 11 flicker frames/min with the draw pipe, 0 with the start script's
  override). Not anti-aliasing, not the live optimization toggles (texture/buffer groups off: same
  rate). The draw pipe already snapshots user data, the walked SRT tables and ring constants at
  stage A, so the race is elsewhere (not found; GOW3_PIPE_VERIFY=<n> reports tables that changed).
  probe_uncharted.py turns multithreaded rendering off for eboot.bin
  (UNCHARTED_U1_MULTITHREADED_TEST=1 keeps it). Live flicker counter: scratchpad gunwatch.py
  (flash = grey-blue ~(54,74,74) where both neighbour frames are near black).
- **U2 `m_lookAt.X() == m_lookAt.X()` (#24, and the user's 16 Oct session):** PlayerBase::UpdateFocus
  (U2 1.02 0x7272e0; point [player+0x2b0], velocity +0x2c0, target rbp-0x60, snap 0x72769a). The
  assert path now clears the velocity and takes the snap when the target is a number
  (FOCUS_NAN_PATCHES). Test (scratchpad focusnan2b.sh): GOW3_EXEC_WATCH=7272e0 gives the player in
  rdi; NaN written to player+0x2b0: without the fix the assert, with it the point is 150.11 again
  3 s later and tracks (155.9 after walking).
- **Frame capture** read buffer contents at guest address 0 (IsValidGpuMapping only checks 40 bits):
  now through TryReadBacking; unreadable buffers are listed as such.

## 18 October 2026, evening: ultrawide Hor+ (#20), #24 = game 1.00, launcher usability (unreleased)

- **Ultrawide (#20) fixed: Hor+ in all three games** (HOR_PLUS_PATCHES, probe_uncharted.py; only
  for render sizes wider than 16:9, UNCHARTED_HOR_PLUS=0 disables). The render camera holds
  zoom = 1/tan(hFOV/2) and aspect = h/w (U2/U3 +0x254/+0x250, U1 +0x294/+0x290); projection
  x = zoom, y = zoom/aspect. Every perspective read of zoom in the camera update and in the
  frustum-plane builder (culling) now gets zoom * min(1, aspect*16/9) through a 37-byte stub
  (vmovss k; vmulss aspect; vminss 1.0; vmulss zoom; jmp back) in a dead assert branch (cave
  starts with int 0x41, which the runtime treats as fatal). Sites (1.02 VAs): U1 0x2c982e,
  0x2c9ac7, planes 0x2ca4ca; U2 0x9dc8fe, 0x9dcb55, planes 0x9dd41d; U3 0x11c1107 (both infinite
  paths), 0x11c11e1, 0x11c1406, planes 0x11c1f21. Orthographic branches (U2 +0x280, U3 +0x268,
  U1 +0x2a4) untouched; U3's sub-pixel offset term at 0x11c15e2 keeps the old zoom (negligible).
  apply_patches gained multi-change variants (pattern, [(offset, original, replacement)...]),
  all or none. Static check: scratchpad horplus_verify.py (disassembly of every stub, no branch
  from outside into a cave). Bench (scratchpad uwtest2.sh, GAME=u1|u2|u3): 3440x1440 on/off in
  all three games, 5120x1440 in U2: wider at the sides, the 16:9 vertical view, edge objects
  drawn, no faults.
- **#24 reporter runs game 1.00** (entry/image sizes match our 1.00 exactly): the 1.02 pattern
  patches are skipped there (U2/U3 focus NaN, FrameBegin wait...). The launcher now asks once at
  Play when the game is older than 1.02 with no update next to it (confirm_without_update), and the
  launch log says so. Red-zone clobbering by Windows exception dispatch was ruled out on Windows 11
  (scratchpad/redzone/redzone_test.c).
- **Launcher:** pages scroll (the Advanced buttons had been hidden since 0.2.2 by the fixed 330 px
  body), more room, GPU-based Recommended preset (gpu_names/gpu_tier), plainer hints, recommended
  choices starred, Advanced tools grouped, double-click a game card to play, #25 direct start.

## 18 October 2026, evening: laptop work merged into this tree

The three laptop sections below were written on the user's ROG Zephyrus G14 (USB copy, G:\uncharted
here) and merged on the desktop on 18 Oct evening: a three-way merge (base: the 0.1.2 export) of the
nine files changed on both sides, the nine laptop-only files copied. Conflict choices: liverpool.cpp
keeps SignalAsync; buffer_cache takes the residency pool and the timeline semaphore; vk_rasterizer
keeps both hang fixes (desktop ReleaseStuckPage after 4 faults, laptop LandWatchedReadback at the
first) and WaitDrawPipeForCapture; image.cpp keeps the GpuAllocationRecords gate with the laptop's
slow-create log and SpareBlockKeeper; probe.c / probe_uncharted.py keep the desktop's later work. One
duplicated declaration block (old-base merge) removed; scratchpad merge_dupcheck.py found no other.
Not merged: D:\uncharted-cmd (command-thread session, unmeasured). Desktop check of the merged build:
U1/U2/U3 single-threaded and U2/U3 multithreaded at 60 FPS, no faults; U3 landed 10 readbacks
through the new watch path. Benchmark runs and scripts: analysis/laptop-bench/.

## 18 October 2026 (night, laptop): Radeon 780M optimization (RTX 5060 switched off)

Laptop: ROG Zephyrus G14 GA403UM, Ryzen 9 270 (8C/16T, 16 MB L3) + Radeon 780M (driver
32.0.21036.1002, Jan 2025), 16 GB, 2880x1800 120 Hz; D: is exFAT; Windows power mode Best
performance. **But G-Helper was in Silent (30 W total, 18 W CPU) and GPU Eco the whole night**
(found 07:50 by the RTX session): every number below is power-limited. The user switched to
Balanced (80 W) and GPU Standard at ~07:50; re-measured numbers follow in a later section.
Measured with a gated scratchpad bench (Continue to the latest save, stand still; keyboard and
mouse off; saves backed up and restored): Frame stats every 5 s, GOW3_PROFILE, GOW3_GPU_PROFILE.
The camera view at the checkpoint varies between runs (1100-1550 draws in U3), so CPU-bound
results are compared in draws per second.

**Fixed / added (all in the laptop copy):**
- **Update 1.02 never ran on the laptop**: exFAT holds no links, so the game view failed and
  every game ran 1.00 (no Bluepoint fixes; JOB_SLEEP, FRAME_BEGIN_WAIT, AIM_RANGE and torch
  patches unmatched). New: `--app0-overlay <dir>` (runtime_file.c): the update is laid over the
  game by the runtime, indexed once, names without case, merged directory listings; probe.c reads
  param.sfo through it. probe_uncharted.py `boot_view` copies what the prepare scripts read
  (executables, sce_module, sce_sys; ~90 MB in analysis/views/boot-CUSA02344-01.02). Used only
  when the link view fails (NTFS unchanged). Log: `Runtime: /app0 overlay ...: 172 files, 33
  folders`. game_update.py links folders first (exFAT fails before copying files).
- **Error 1450 at start (16 GB PCs)**: the 6080 MiB guest pool was a SEC_COMMIT section (the
  whole pool charged against the commit limit at start). Now SEC_RESERVE, committed per direct
  allocation / flexible map (`host_commit`), retrying up to 10 s (lock released while it sleeps)
  while a system-managed page file grows.
- **Flexible memory split (pre-existing, found by review)**: split_at kept the parent's phys for
  the right piece of a flexible VMA, so after a partial mprotect/munmap of flexible memory backing
  copies hit the left piece's pages and flex_free zeroed live memory (reproduced with a harness).
  Fixed for every backed kind. Candidate for random guest corruption on every PC.
- **AMD allocation stalls**: vkAllocateMemory takes ~0.6 ms/MiB on this driver (170 ms per
  256 MiB VMA block). New: `Slow image create` log; image.cpp SpareBlockKeeper holds a 64 MiB
  spare so the next images find a block (after an image burst; GOW3_SPARE_IMAGE_BLOCK=0 off);
  buffer arenas take resident ranges from 64 MiB chunks allocated ahead on a thread
  (GOW3_RESIDENCY_CHUNKS=0 off): level-start frames with >10 ms residency 10 -> 0. One ~170 ms
  image-block stall at level start remains (120 images in one frame).
- **Side-queue page fetches** (CPU writes beside GPU data): a timeline semaphore instead of a
  fence (vkResetFences is a kernel call on AMD: 6% of the U3 command thread).
- **U1 shader compiles**: synchronous since 14 Oct (dark interiors); the 15 Oct small-target
  rule (SmallTargetDraw) covers the LUT draw, so U1 compiles in the background again (froze
  1-2.4 s per new area; now worst frame ~120 ms). UNCHARTED_U1_SYNC_SHADERS=1 restores it.
  To check: U1 temple torch light.
- **Multithreaded rendering races** (pre-existing): readbacks landed twice (`AddDelta: Not enough
  watchers`, `FreeDeferred on an allocation that is not held`, STATUS_HEAP_CORRUPTION) in U3
  within 1-3 minutes on the laptop. Fixes: a shader capture for the command thread waits for the
  draw pipe to run dry before handling its fault (WaitDrawPipeForCapture); the pipe ring has one
  producer again (RunInOrder from the recording thread runs inline, from any other thread, e.g.
  an EOP flip signalled on a copy thread, goes through the command thread). The watcher assert
  is a logged no-op. GOW3_FEWER_DRAINS=1 (off by default): fewer draw-pipe drains.
- Diagnostics/tests: GOW3_RING_STATS=1, GOW3_GPU_THREAD_PRIORITY / GOW3_GPU_THREAD_CORE (no
  effect measured), GOW3_EQUEUE_SPIN_US=<n> (waiter spins before sleeping).

**Measurements (1.02, 1080p, fullscreen 2880x1800, job sleep on, frames queued 1):**
- Levels are CPU-bound on the GPU command thread: U3 Cartagena ~54K draws/s single-threaded (49
  FPS at 1100 draws, 35 at 1530), U2 ~57K (36 FPS at 1600), U1 ~95K (37 FPS at 2620). Menus are
  GPU-bound (U3 50-55, U2 42-50).
- Multithreaded rendering, final build (race fixes): U3 43.9 FPS vs 35.4 single-threaded in the
  same 1540-draw view (~68K vs 54K draws/s, +24%); U2 no change; U1 slightly slower (34.6 vs
  36.5). No readback crash in 4 multithreaded runs since the fixes (it crashed 3 of 3 U3 runs
  before them). GOW3_FEWER_DRAINS=1 is about as fast; left off. But 2 of 5 multithreaded U3 runs on the
  final build stopped flipping right after the level loaded (0 of 2 single-threaded): suggested
  off for now.
- 2 frames queued: menus +10%, but U2 levels 9 FPS and U1 27 FPS (ray-cast fences landed 71 ms
  late): keep 1.
- GPU (U3 level, 780M, 1080p): ~17-18 ms/frame; 60 FPS on the 780M needs ~900p + FSR 1.
- **Memory pressure (16 GB + 780M)**: the 780M's "video memory" is system RAM. During U2 the
  system had 177 MB of commit left (game ~4.3 GB private + ~5 GB guest section + GPU memory, plus
  ~3 GB of other apps), and that run had a 2.1 s frame. U3 stopped flipping right after the level
  loaded in 5 of ~10 runs late in the night (both single- and multithreaded, process alive, no
  log message): the desktop's 17 Oct "U3 stops flipping on the AMD iGPU" (job fibers waiting in
  an allocator). Likely made worse by memory pressure; not resolved. Close other programs; the
  RTX 5060 (own 8 GB) avoids most of it.
- Experiments that did not help or broke things: GPU command thread priority/core (no effect);
  routing RunInOrder from other threads through the command thread (hangs: the command thread
  can wait for the flip it would queue; reverted); GOW3_EQUEUE_SPIN_US (a run with it stopped
  flipping; left off by default).
- Command thread (single-threaded U3 level): compute 54% (dispatch binding 28%, compute release
  fences 19%: signal + interrupt + event wakeup ~16%, waiting for deferred signals 7%), draws 30%;
  BindBuffers/ObtainBuffer 24%, AMD driver ~10%. Per-draw constant copies were 23-29% of stage A
  with multithreaded rendering.

## 18 October 2026 (morning, laptop): U3 "stops flipping" on the 780M - cause found and fixed

The hang behind "multithreaded rendering off on the laptop" (and the desktop's 17 Oct "U3 on
the AMD iGPU stops flipping"). Found from two hung runs with full minidumps and lldb stacks
(scratchpad hang-catcher: no Frame stats line for 25 s -> MiniDumpWriteDump + `lldb -p`).

- **Where it hangs**: not "after the level loaded" but at the menu -> level-load transition
  (Continue): every laptop hang (7 overnight runs, 2 today) stopped in Frame stats window 5-6,
  before the loading screen's first window, right as cs 67921bad/c92150ba run for the first time.
  It hit single-threaded too (2 of ~12 overnight), with multithreaded rendering more often.
- **What hangs**: one NdJobWorkerThread sits on a single guest store (+0xea50b1, `movl $0,0x10(%r8)`,
  zeroing a new pool item) for the whole hang: 20-22 s kernel time, ~6 s user, i.e. an endless
  access-violation loop. Its page is PAGE_NOACCESS. Every other thread waits for that job: the
  main thread polls the job counter (WaitForCounter usleep loop, +0xd9756a), another worker spins
  on a guest lock the stuck job holds (+0xea611e), the GPU command thread sleeps in
  Liverpool::Process (no commands, no deferred fences), recorder and draw pipe idle.
- **Why the page stays NOACCESS**: it is the `watched` range of the only pending buffer readback
  (dump 1: tick 4316, watched 0x1065c56000+8 KiB, GPU piece 0x1065c56d60+1 KiB, the store at
  0x1065c57180 is just past it; dump 2: tick 4297, watched 0x1065c72000+8 KiB, store 0x1065c732e0).
  Read watches (EOP/ray-cast readbacks, "read-protected until copied") make a page inaccessible
  until the readback lands (PageState::Perms). A CPU **write** fault there
  (Rasterizer::OnWriteFault -> InvalidateMemory -> memory tracker) marks the page CPU-modified and
  reports "handled", but nothing lifts the readback's watch: the store faults again, and again,
  until the GPU command thread lands that readback. It only polls while it processes work or
  while fences are deferred to readbacks; here it had none (deferred_signals 0, cached GPU tick
  4314 < readback tick 4316 < current 4317: the GPU finished long ago, nobody looked). The guest
  submits nothing until that job finishes: livelock.
- **Why the 780M**: the readback is still in flight when the job initializes its item beside the
  GPU's 1 KiB result block (false sharing in one page); on the RTX it has normally landed during
  command processing. Multithreaded rendering records the readback later in the submission, so
  the command thread goes idle with it pending more often. Not memory pressure (no commit-limit
  messages in any U3 log; commit headroom fell to 270 MB in clean runs too), not RunInOrder, not
  equeue wakeups.
- **Fix** (vk_rasterizer.cpp/.h, buffer_cache.cpp/.h, page_manager.cpp/.h): after a write or read
  fault is handled, Rasterizer::LandWatchedReadback checks whether a readback still read-protects
  the page (PageManager::IsReadWatched) and, if so, lands the readbacks that watch it
  (BufferCache::LandReadbacksWatching -> CompleteReadbacks(wait, tick); on the GPU thread through
  SendCommand<true> for guest threads, inline on GPU-side threads). Watches start only after the
  readback's submission, so the wait is for work already sent to the GPU. The retried store then
  succeeds. Logs "Readback watch: a fault at ... landing the readback (n so far)" (first 8, then
  powers of two) and warns if a page is read-protected with no buffer readback owning the watch.
- **Results** (U3, multithreaded rendering, job sleep on, 1080p, 780M as GOW3_GPU_ID=0 with the
  RTX on; each run: boot, Continue, load, 40 s standing in the level, which covers every hang
  seen; the desktop's struck within 35 s of the level start):
  - Before (05:27 build): 2 of 8 runs this morning hung (07:46, 08:11), both at the transition;
    overnight 7 of ~25 U3 runs, 2 of them single-threaded.
  - After (08:43 build and the 08:48 rebuild that includes it): **7 of 7 runs clean** (08:43-08:48
    in Balanced 80 W, 09:12-09:18). In every run the new path ran 65 to 500+ times, starting in the
    transition window, and 1-3 times per run at exactly the two stores that livelocked in the
    dumps (0x1065c57180, 0x1065c732e0). No "no pending buffer readback" warning, no errors.
  - RTX 5060 runs on the same build (other session): 1-17 landings per run (the readback has
    usually landed before the guest touches the page), U3 multithreaded 60 FPS, no errors.
  - Level frame rate unchanged within noise (43.6-45.7 FPS at 1116-1535 draws, once 40.9); the
    long frames left are the level-start ones (shader compiles, the ~200 ms image-block stall).
- **So**: multithreaded rendering no longer needs to be off on the laptop for this reason (it gave
  U3 +24% overnight, measured in Silent mode). The command-thread session reported that every U3
  run of its own hung at level start before this fix.
- Tools (u3-hang session scratchpad): hangrun2.sh (gated run + hang catcher + per-second memory
  log + early stop), minidump.py, mdump.py (minidump reader: fibers, threads, memory), dis.sh
  (guest disassembly). Memory note: reference-hang-dump-analysis.
- **Follow-up (09:20, stopped by the user before verification)**: an independent review (handbook
  session) found a multithreaded race the fix makes more frequent: Liverpool::ProcessCommands
  drained the draw pipe once before its command loop, so a command queued after that check (it is
  also called inside the packet loops, while the recording thread is busy), or one behind a flip
  command that woke the recording thread, could land readbacks concurrently with it (double
  landing, as before the 18 Oct night fixes). liverpool.cpp now drains before each command. Built
  at 09:21 (out/gow3-probe.exe) but **not yet run**: the next session should repeat a few
  multithreaded 780M runs and the RTX single-threaded 1440p case.
- The hang is not 780M-only: before the fix it also hit on the RTX 5060 single-threaded at 1440p
  (RTX session, run u3-rtx-st-1440-1018-075834: three level-start frames, the last with 27.9 ms of
  write faults, then no flips) and twice single-threaded at 1080p in the command-thread session
  (08:03, 08:07). Same symptom; consistent with the cause (the GPU still busy with the readback
  when the job writes beside it).

## 18 October 2026 (morning, laptop): power mode, 80 W numbers, session coordination

- **Power mode**: G-Helper was in Silent (30 W total, CPU 18 W) with GPU Eco from 17 Oct 23:58
  (found by the RTX session at ~07:50). The user switched to Balanced/Turbo (80 W) and GPU
  Standard. With the RTX on, the 780M is GOW3_GPU_ID=0 (`gow3-gpu-capabilities.exe --list`).
- **780M at Turbo, single-threaded, job sleep on** (build 05:27): CPU-bound levels barely moved
  (U3 34.9 FPS at 1517 draws = 53K draws/s vs 54K in Silent: one busy thread fits even in
  18 W). U3 900p + FSR: menu 55.5, level 36.0 at 1534 draws; U2 900p + FSR: menu 53.7, level 40.3
  at 1606 draws (65K/s); U1 1080p: level 36.5 at 2621 draws (96K/s). U2 at the user's 2560x1440
  on the 780M: level 27.2 FPS with 13 hitch windows (menu 37): use 900p + FSR there.
- **Output size is not the 780M's limit**: U3 in a 1920x1080 window was no faster than 2880x1800
  fullscreen (menu 54 vs 53); 720p + FSR locks the menu at 60. The GOW3_FULLSCREEN_RES experiment
  was reverted.
- **Launcher (with the user's approval)**: job_sleep "on", multithreaded off for u1/u2/u3,
  frames_queued 1 (backup: %APPDATA%\Uncharted Launcher\launcher.json.bak-20261018). Not changed:
  resolutions (U2 is still 2560x1440).
- **Coordination**: several sessions share D:\uncharted: analysis/laptop-bench/SHARING.md, a
  first-come-first-served lock for builds and game runs (lock.sh take/try/release/leave/show,
  QUEUE), EDITS.md for files in progress. A review of the U3 hang fix found a draw-pipe race
  (ProcessCommands drained once before its loop; a command queued after it, or behind a flip that
  woke the recording thread, could land readbacks while that thread did); the u3-hang session
  made it drain before each command (built 09:21, not run yet).
- **Remaining for 60 FPS**: levels need ~1.7x the command thread's throughput (U3 ~91K, U2 ~96K
  draws/s at their draw counts). The command-thread session's changes (D:\uncharted-cmd, RESUME.md:
  readback `rewritten` clipping, batched compute interrupts, a stream-copy thread, smaller items)
  are built but not measured. Multithreaded rendering gave U3 +24% before the hang fix; re-check
  with the hang and drain fixes. All sessions were stopped at the user's request at 09:23.

## 18 October 2026 (21:15): host crash in TextureCache::FindDepthTarget fixed

The user's U2 session on the merged build ended after 38 min: `Fault: read access to 0x7dd5edd0 at
Uncharted.exe+0x11474b` (GPU command thread; FindDepthTarget <- BeginRenderingFull <- DrawRecord).
FindDepthTarget held `Image& image` across `slot_images.insert` (the stencil placeholder); SlotVector
Reserve moves every image and frees the old storage when it grows, so `image.image_uid` (rbx+0x58)
and `image.FindView` read freed memory. Pre-existing (not from the laptop merge). Fixed by fetching
the image again after the insert; ResolveDepthOverlap had the same pattern (`cache_image` used after
inserting the recreated depth image) and is fixed too. ExpandImage, FindImage and ResolveOverlap
fetch references after their inserts (fine). Smoke: U2/U3 60 FPS, no faults.

## 18 October 2026 (21:35): U3 freeze = endless nested fault in the stuck-page check (since 0.2.0)

The user's U3 froze (0 CPU, 146 threads); lldb (scratchpad freeze-u3-stacks.txt): the GPU command
thread nested in GuestFaultSignalHandler -> cached_pages[addr >> 12] -> fault -> the same handler,
again and again. Shader d701fef2's SRT walk read through garbage user data (0x6f36c37e6c..., then
non-canonical addresses, reported as fault address -1); after 4 faults on that "page" the 18 Oct
morning stuck-page rule (fault_repeats >= 4) indexed the page table with -1 >> 12. Fixed: the rule
only looks at addresses below NUM_ADDRESS_PAGES (as the laptop's IsReadWatched already did). The
walker then skips such loads as before. Smoke: U3 60 FPS, no faults.

## 18 October 2026 (22:00): long-session safety net

- **Run history** (scratchpad run_history.py over analysis/<game>/attempts, 1,019 kept runs): every
  user crash/freeze signature of 15-18 Oct is fixed or not seen since: FrameBegin m_gfxEopTick
  int 0x41 (U2 0xbd444a x9, U3 0xe3b8fa x11 bench; FRAME_BEGIN_WAIT_PATCHES), U1 host fault =
  my frame capture (fixed), ntdll+0x1ff2a (16 Oct MT work), GPU-library STOPs (14-15 Oct),
  FindDepthTarget and the nested-fault freeze (fixed today).
- **Fault-handler depth guard** (probe.c vectored_fault): a fault nested more than 8 deep inside
  gow3gpu_handle_fault reports and stops instead of recursing until the stack runs out.
- **Hang watchdog** (probe_uncharted.py hang_watchdog + tools/hang_report.py): no Frame stats line
  for 30 s (UNCHARTED_HANG_SECONDS; needs diagnostics) -> every thread's state from the launching
  process: StackWalk64 with the modules' unwind tables (module+offset), guest rbp chains, thread
  names, grouped by stack -> analysis/<game>/hang-<time>.txt (newest 10; ~20 KB, 0.2 s), copied into
  the run's attempts folder, named in boot.log after the game ends; minidump in dumps/ in the source
  tree (UNCHARTED_HANG_DUMP; ~340 MB, newest 2). Not written into boot.log while the game runs: the
  game writes through its own file position and overwrote appended text. Launcher: "The game
  froze" message whenever a report exists (also after STOP). Tested by suspending a bench game
  (scratchpad suspend_game.py): report + resume clean; offsets symbolize with llvm-addr2line.


## Fork (espindoIa) notes, merged onto 0.3.0 (19 Oct)

Merge choices (details in the sections below and in CHANGELOG.md):
- Temporal AA/upscaling: 0.3.0's structure (always prepared, menu switches live, upscaler=off by
  default) with the fork's camera source (GOW3_CAMERA_SOURCE=flat + object motion vectors), so
  the FSR/DLSS quality modes work; 0.3.0's velocity-pass camera (GOW3_VELOCITY_PS=f4b18264) via
  UNCHARTED_MOTION_SOURCE=velocity, Native AA only (TemporalUpscaler forces preset 0 in that mode;
  Gow3Settings::upscale_modes_available hides the Mode row). Menu and launcher: one Anti-aliasing
  choice + Mode (preset) + DLSS model (dlss_model); the fork's pending-restart logic is gone.
- Cutscenes: probe.c sets GOW3_MOVIE_PACED=1 when the movie_hook stub installs; VideoOut's 60 Hz
  movie vblank (0.3.0) then stays off and is the fallback otherwise.
- Job workers: 0.3.0's JOB_SLEEP_PATCHES (launcher Idle game threads) + the fork's 1 ms wait;
  the fork's WORKER_SLEEP_PATCHES (same bytes) removed. FrameBegin assert: 0.3.0's bounded wait
  (FRAME_BEGIN_WAIT_PATCHES); the fork's unbounded `pause; jmp` patch removed (it was applied
  first and kept 0.3.0's signature from matching).
- IsMapped cache: the fork's (generation bumped before and after map/unmap, Gow3Toggle switch);
  ring copies: the fork's reuse plus 0.3.0's GOW3_RING_STATS; exec watches accept both r: forms.
- nvngx_dlss.dll stays out of the package (launcher adds it); multithreaded rendering on by
  default in all three (0.3.0 forced it off in U1 for GitHub #23's grey guns; the user wants it
  on, 19 Oct; the fork's draw-pipe ordering fixes may cover #23, not verified).

US version CUSA02320 + update 1.02 (19 Oct, the fork's own copy now: Jogo\CUSA02320 and
CUSA02320-patch, no uroot):
- From source after the 0.3.1 merge, 35-40 s boots of eboot.bin, big2 and big3 (probe_uncharted.py
  --boot): "Update 01.02" found, no "NOT applied" patch, the runtime hooks installed (movie pacing
  U1/U2/U3, mouse look), frames at the 120 FPS cap, no files.json / ASSERTION. The player's 0.3.1
  package also played U3 with it (13:22 run: saves mounted, 46 s, exit 0).
- So upstream's #29/#43/#48 stop (files.json -> not found, ASSERTION root, ingame-shaders.cpp) does
  not reproduce with this dump. Not verified: those reporters' dumps (their bin.psarc matched
  upstream's hashes), long play on US 1.02, and whether the menu path (Collection menu -> game)
  differs from direct starts.

Upstream 0.3.1 merged (19 Oct), choices:
- One frame start per frame (Supermedo #261): applies to U1 and U3 only. U2 (FlatSource) keeps the
  fork's display_frame_open gate (four display draws a frame; the first after the scene starts it).
  Not measured after the merge: frame starts per shown frame in U1/U3.
- Toggle bits: upstream's OneFrameStart (59) and FullScreenPassJitter (60) are 1 << 61 and
  1 << 62 here; 59/60 stay the fork's IsMappedCache and RecursiveLockArray.
- Movies: 0.3.1's fitted size (black bars at 16:10/ultrawide) with the fork's Lanczos filter.
- Launcher settings version stays 7 (the fork's multithreaded default) with 0.3.1's Wine
  game_dir_unix step; package ships play.py and install_update.py + PkgTool.

Upstream 0.4.0 merged (19 Oct, upstream/main 6318dfb), choices:
- Taken as is:
  - runtime_memory.c maps guest ranges as views of at most 2 MiB (#52, GOW3_VIEW_CHUNK_MB=0
    for one view), plus probe.c's cover_current_stack bounding guest stacks by their mapping.
    The fork's protection paths already work per region (runtime_memory.c protect loop,
    carve_host's saved runs), so no fork code assumed one view per mapping.
  - Map/unmap timing lines ("memory map time", "GPU: unmap time").
  - GOW3_COPY64_EOP_READBACK for Drake's Fortune (#6), and the U1 magazine cheat patch.
  - The FSR 1 pass's input_image_size/LastOutput, and fetch_aa_files.sh fetching all FSR 4
    presets.
- Labels in compute RELEASE_MEM (#51): both sides wrote them through the backing view. The
  fork's version and comment are kept.
- Upscaling (kuraeow, upstream PR 50; 61c3742, 98e9b06):
  - The code is in. TemporalUpscaler::OnComposite and composite_frames run only with the
    velocity camera (camera_motion.VelocityMode()) and GOW3_RENDER_RES.
  - probe_uncharted.py sets UNCHARTED_OUTPUT_RES / GOW3_RENDER_RES / GOW3_UI_COMPOSITE_PS only
    when UNCHARTED_MOTION_SOURCE=velocity. With the fork's flat camera, GOW3_RENDER_RES would
    have put the upscaler on its old compatibility path (scaled_session). Without this gate,
    Drake's Fortune at 4K (render 2880x1620) set it.
  - The rasterizer's frame start keeps the fork's FlatSource gate (display_frame_open) inside
    upstream's frame_start lambda and !CompositeFrames() condition.
  - The launcher, README and overlay text stay the fork's: Anti-aliasing + Mode + DLSS model,
    the "Upscale with FSR 1" toggle.
  - Upstream changes to RunScaled that also reach the fork's quality modes:
    - LdrFormat: 8-bit sRGB inputs are read through UNORM views, and the UI image is UNORM.
    - TAA resolves at the render size, then FSR 1 or a bilinear blit brings it to the output.
- Not taken:
  - upstream's switch turning multithreading off for Uncharted 3 (UNCHARTED_U3_MULTITHREADED_TEST);
  - its Drake's Fortune MT-off block, which was already dropped at the 0.3.0 merge.
- Release notes: tools/release-notes/v0.4.0.md stays the fork's. Both repositories now have a
  0.4.0, so the next fork release needs a version name that cannot be mistaken for upstream's
  (ROADMAP.md, 0.5).
- Checked after the merge with tools/play.py: 4K, multithreading on, DLSS model M, Continue
  into gameplay, 15 s measure. All three images were checked by screenshot.

  | Game | Settings | Result |
  |---|---|---|
  | U3 | DLAA | 64.7 FPS, p99 24.5 ms, GPU 95%. DLSS gets HDR input, so LdrFormat is not on this path. |
  | U3 | DLSS Quality (GOW3_UPSCALE_PRESET=1) | 2560x1440 -> 3840x2160, HDR input, 85.2 FPS, p99 19.5 ms, colours right. |
  | U1 | DLAA | Renders 2880x1620 as before; no "Upscaling:" line, so the velocity gate holds. 92.4 FPS, GPU 83%. |
  | U2 | DLAA | 67.6 FPS, GPU 96%. |

  U1's map log: "205 unmaps 6.4 ms" in total, against ~23 ms for each unmap before #52's
  chunked views. No ASSERTION and no "NOT applied" patch in any game.
- Not checked:
  - the grenade launcher scene (#6);
  - #52's museum cutscene in U3;
  - the velocity-camera upscaling path (UNCHARTED_MOTION_SOURCE=velocity with
    UNCHARTED_OUTPUT_RES);
  - FSR 4 and TAA in the quality modes.
- Python tests: 4 errors that are not caused by the merge and that touch no file it changed:
  God of War modules (gow3_vulkan.py, save_backup, upscaler assets) and symlink privilege.

### U3 multithreaded freeze / "Invalid packet" (FIXED 19 Oct)
- User: U3 "crashes" seconds into gameplay (port package, 4K, 120 FPS, DLAA model M, MT on). The
  run's log simply stopped, exit code 0: the window thread's _Exit(0) after a close request (the
  frozen window closed by hand). shim/window.cpp now logs "Window: closing (...)".
- Repro (Continue -> chapter 1 gameplay, C:\dev\repro_u3b.sh): pre-merge build froze (game threads
  in sceKernelWaitEqueue, GPU threads idle: thread dump via tools/hang_report.thread_report);
  merged build stopped at 111 s, "Invalid packet ... in a command buffer ... Unreachable code"
  (the game had reused a command buffer stage A was still parsing). MT off: 6 min clean.
- GOW3_TEST_OVERTAKE_LOG=1 (0.3.0 diagnostic): end-of-shader labels, WRITE_DATA and the flip
  interrupt reached the guest while an earlier EOP fence was held for its ray-cast readback
  (BufferCache::DeferUntilLastReadback); WaitDeferredSignals only covers the recorder-thread
  signals. Fix: liverpool.cpp BehindHeldFence queues them on the same readback (label/value
  passed, so LandDeferredFenceFor still answers GPU waits on them). After: 491 s at the user's
  settings with MT, no freeze, 66-78 FPS (52 without MT); remaining overtakes: compute-queue
  releases (another queue: no order on PS4 either) and 2 DMA writes (left: deferring a DMA off the
  recording thread is unsafe; not seen to matter). U1 and U2 boot and play with MT (U2 60 FPS).
- Also found: VideoOut's 60 Hz movie fallback read GOW3_MOVIE_PACED at PresentThread start, before
  probe.c installs the hook and sets it, so it was always on (cutscenes at ~13 FPS in a 4K run);
  now read when the first movie plays. Cutscenes are 1080p movies drawn as one quad (2 draws a
  frame, no upscaler run): their 4K scale (videodec2 WriteMovieCopy) is now Lanczos.

### Scripted play, four more multithreaded freezes, fault prewait (19 Oct, evening)
- tools/play.py: start/stop a game, pad tokens held for a time (sticks as directions or x,y),
  a fresh shot after each action (or `--shot-at` inside the hold), every input recorded as a
  GOW3_PAD_REPLAY route (`mark`/`save`, `replay`), `measure` (GOW3_FRAME_TIMES percentiles,
  nvidia-smi, PresentMon when the account may trace: it needs admin or the "Performance Log
  Users" group, S-1-5-32-559), `compare` (SSIM in numpy, changed pixels, heat map).
  GOW3_LIVE_VIEW=<dir>: the presenter blits the presented image to 960 wide every 200 ms
  (GOW3_LIVE_VIEW_MS/_WIDTH), written as live.bmp/live.txt off the GPU threads. The pad file is
  re-read on its 100 ns write time on Windows (stat's whole seconds missed same-size edits).
- Bench (C:\dev\bench: bench.sh, ab.sh): player's U3 ini on all three games (4K, DLAA, model M,
  120 FPS cap, MT on), saves restored before each run, Continue into gameplay, stand still.
  U3 chapter 3 (Cartagena street) before: 46.9-50.8 FPS, p50 20.9 ms, p99 34.6 ms, GPU 72-78%.
  GPU profile there: 20.6 ms/frame of which 4.9 ms idle; DLSS evaluate (DLAA, model M, 4K) 7.8 ms,
  cs 1c957b01 1.3 ms (one 1024-thread group, LDS sort), 1231 tiny cs 8f067540 0.7 ms. GPU ceiling
  at these settings ~64 FPS. GPU command thread (stage A) profile: 37% draining the draw pipe
  (compute IndirectBuffer 23.8%, LandDeferredFenceFor 10.9%), 21% SideQueueFetch for guest writes
  into GPU-written pages (~140/s, ~1.4 ms each waiting for the GPU), real work ~30%.
- Freezes found by the bench (hang watchdog thread reports, llvm-addr2line), all fixed:
  1. Compute RELEASE_MEM label written by the Vulkan recording thread with a plain store into a
     protected page: fault -> download through the GPU command thread -> drain -> flip on the draw
     recording thread waiting for the recording thread. Labels there now always go through the
     backing view (as SignalEop does); seen twice (GPU-written page, then CPU-write tracking).
  2. IrqController::Signal re-entered on one thread (flip handler -> PrepareFrame poll -> held
     fence -> GfxEop): second lock of the same mutex. Nested interrupts are delivered after the
     handler.
  3. Draw recording thread faulting inside TextureCache::FindImage (texture cache mutex held);
     the inline fault handling landed readbacks, a held fence raised GfxEop, the flip handler's
     PrepareFrame locked the same mutex. Interrupts raised during a GPU-side fault are held
     (IrqController::HoldScope) and delivered at the thread's next safe point (draw pipe packet
     end, ProcessCommands, command processor idle).
  4. Caused by 3 (seen once, MT off, on the title screen): an end-of-pipe interrupt held that way
     was the one that submits the flip (sceVideoOutSubmitEopFlip's handler runs
     SubmitFlipInternal inline), and the GPU command thread then blocked in WaitVoLabel for that
     flip's label without passing a safe point. Held interrupts are now delivered before that
     wait too.
- Fault prewait (toggle file second word, bit 1 = off): a guest write fault into GPU-written
  memory waits on its own thread for the GPU work that wrote the page (gpu_write_window/page_tick,
  submitted ticks only, 50 ms bound) before the GPU command thread fetches it. ~160 per 5 s, ~8 ms
  each on the guest thread. A/B in one session (alternating 15 s windows): see CHANGELOG numbers.
- Tried and dropped: no drain for compute IndirectBuffer packets (55.5 vs 55.3 FPS; the waits moved
  to SyncSrtTables' pipe_write_ranges check: the graphics SRT walks read what the dispatches
  write). pipe_write_ranges overflows at 256 dispatches (U3: ~1,230 a frame), after which every
  walk drains. Both measured together at DLAA model K (CPU-bound, 4 states x 2 rounds in one
  session): merging ranges took the SRT drains from ~1,040 to ~94 a second and all waited drains
  from ~4,000 to ~1,250, but FPS stayed (base 61.4/61.2, merge 60.8/60.5, nested 61.4/60.1, both
  62.2/58.7; p99 within 20.6-23.4 ms everywhere): stage B (draw recording) is then ~90% busy and
  stage A waits for it instead. Not kept; useful again once stage B is faster.
  Measured again at the player's DLAA model M (GPU-limited, same protocol): base 54.8/54.9 FPS
  (p99 28.8/25.2), merge 56.1/54.6, nested 55.5/56.4 (p99 24.5/23.7), both 55.8/55.5. The nested
  no-drain is kept (default on, toggle second word bit 4 = off); the merge adds nothing over it
  and stays out.
- Settings, same scene, current build: DLSS evaluate (DLAA 4K) model M 7.8 ms, model K 2.2 ms.
  DLAA K: 60.8 FPS, p50 15.9, p99 23.6 ms, GPU 64% (CPU-bound); DLSS Quality M (2560x1440):
  60.5 FPS, GPU 70%. Before/after at the player's settings (same protocol, 0.3.0 package copy vs
  current): U3 49.8 -> 54.1 FPS (p50 19.8 -> 17.8, p99 35.6 -> 29.9), U1 93.9 -> 94.0 (GPU 93%,
  renders 2880x1620: 4K targets do not fit its GPU area), U2 64.1 -> 63.5 (GPU 94%).
- Checks (current build, player's settings):
  - Vulkan validation (SDK 1.4.363, sync validation), U3 into gameplay and ~1 min of walking:
    MT on and MT off give the same 7 ids (VUID-vkCmdDraw-None-09600, GraphicsPipelineCreateInfo
    layout-07988/07990, RuntimeSpirv OpEntryPoint-08743/Location-06272, SYNC-HAZARD-WRITE-AFTER-
    WRITE and READ-AFTER-WRITE on the 1920x1080 D32S8 depth at 0x10513e8000: a layout transition
    after vkCmdEndRendering without LATE_FRAGMENT_TESTS in srcStageMask, and a LOAD after a
    transition whose dst was SHADER_READ only). Pre-existing, not multithreading; not fixed.
  - U1 grey guns (#23): 60 s aiming the pistol and turning in the jungle (Sully), every live frame
    (~20 ms) kept; one-frame flashes counted as 16x16 blocks more than half differing from both
    neighbours while the neighbours agree (C:\dev\bench\livecap.py). MT on: 15 of 1132 frames
    with 2+ blocks, MT off: 23 of 658, all aim/unaim camera changes where the capture skipped
    frames; the pistol always textured. GOW3_PIPE_VERIFY=16 and GOW3_RING_REUSE_CHECK=1 during the
    MT run: 0 resource tables changed after stage A, 0 register copy differences, 0 of ~1.7M
    reused ring copies per 5 s differing. Not reproduced in that scene (the user's report was a
    gunfight; the boats were not reached).
- DMA overtakes (GOW3_TEST_OVERTAKE_LOG): decision to leave them. DMA_DATA already waits for the
  recorder-thread copies and fences (WaitHostCopies/WaitDeferredSignals); what it can pass is a
  fence held for a ray-cast readback, which on the PS4 is an end-of-pipe write that completes after
  the CP has already executed later DMA_DATA packets; holding the DMA instead would feed later
  draws stale data (its destinations are read by the following GPU work).
- Frame pacing (user: "o frame pacing parece bem ruim"), Uncharted 2 at the player's settings
  (VSync on = FIFO, 164 Hz display, ~65 FPS, GPU-bound). New diagnostics: GOW3_FRAME_LOG kinds Q
  (flip queued after the fences before it) and C (guest saw it complete), steady-clock times;
  GOW3_DISPLAY_LOG=<file>: on-screen times from VK_KHR_present_wait (enabled only then), and
  <file>.gpu: when the GPU finished each flipped frame (its ready tick). C:\dev\bench\timeline.py
  merges them. Findings:
  - The guest submits flips evenly (S p50 14.7, p99 ~26 ms); the port held each flip behind the
    ray-cast readback fences before presenting it (QueueFlipInOrder), so presents came in bursts:
    38 ms, then two 0.1 ms apart (port flip intervals p99 39 ms, 1% low 25 FPS). GOW3_FRAMES_AHEAD
    0/1/2 changed nothing.
  - Fix 1 (toggle second word 8): the frame goes to the present thread right after PrepareFrame;
    only the guest-visible flip (CompleteFlip: status, events, labels) waits for those fences, and
    never before its present (the frame limit keeps holding the guest).
  - One session, 12-15 s windows, two each: port intervals p99 39.0/38.8 -> 24.0/23.7 ms, 1% low
    25 -> 40 FPS, FPS unchanged (65.8 vs 65.6). On screen (present wait): off p99 30.5/29.8 ms,
    sd 7.4/6.9, 5.8% of frames reported on screen under 5 ms after the previous one (less than
    one 164 Hz refresh); fix 1: p99 29.6, sd 6.8, 0.5%. Uncharted 3 (same protocol): FPS 54.8 vs
    54.9, port p99 29.2 -> 25.5 ms, on screen unchanged (p99 26, sd 2.7, already even).
  - Tried and dropped: presenting on a third queue of the graphics family (the blit then waits
    for its own frame only, not for the next frame's work on the main queue). U2 on screen sd
    6.8 -> 6.0/5.9, but U3 worse in all three windows (sd 2.7 -> 3.2-3.4, intervals over 25 ms
    4% -> 7.6-8.4%).
  - Left: about every fifth frame the GPU finishes two frames within ~1 ms (.gpu log), although
    each frame's last batch was submitted ~15 ms apart; ~14% of U2's on-screen intervals are still
    over 25 ms. Explained and fixed below (fix 2).
  - Cause (GOW3_SUBMIT_LOG=<file>, new: every draw/present/flip submission with its CPU phases,
    GPU start/end timestamps and done time; PrepareFrame F0-F3, Present A0-A3, flip packet N,
    SignalFlip I (1 = held), deferred interrupt J, SubmitFlip S; C:\dev\bench\subpace.py). The
    late frames are end-of-pipe flips held behind a ray-cast fence: the flip packet reaches the
    recording thread (I 1), its interrupt fires 3-18 ms later when the readback lands (J), and only
    then did PrepareFrame submit the frame's last batch (post-process; its ready tick). By then the
    next frame's largest submission (~9.5 ms of GPU work, submitted by the GPU command thread right
    after its flip packet) was queued ahead of it, so the ready tick finished 0.1 ms before the
    next frame's: e.g. tick 12567 GPU -0.6..8.9 ms, then ticks 12568 (frame N) and 12569 (N+1) done
    at 9.2/9.3 ms. Ruled out by the same log: GetRenderFrame never waited (p99 0.0 ms), host-copy
    waits in submissions p99 0.0 ms, recording syncs p99 0.1 ms, 2 of 18684 submissions took
    another tick than the one at their start, GPU idle between submissions p50 0.
  - Fix 2 (toggle second word 16): an end-of-pipe flip's frame is prepared and presented when its
    flip packet is reached in stream order (SignalFlip -> VideoOutDriver::PrepareNextEopFlip, the
    flips registered in order at sceVideoOutSubmitEopFlip); the interrupt then only completes the
    flip (status, events, labels), still after the readbacks. One U2 session, 7 s windows, three
    each: on screen >25 ms 0.2/0.4/0.6% vs off 17.8/20.3/17.5%, p99 24.1/19.9/19.6 vs
    31.3/31.6/31.3 ms, frames per window 463/465/468 vs 433/439/429 (~66.5 vs ~62 FPS). U3, one
    session, 5 s windows (scene heavier as it ran, both states): >25 ms off/on 2.3/4.2, 4.9/5.6,
    14.2/14.7, 78.6/51.3%, frames 258/260, 247/252, 226/225, 126/158: no change beyond noise.
    Image checked at the U2 bench view. Not checked: U1 (single-threaded rendering by default, so
    the flip is not run from the draw pipe there).


## 18 October 2026: performance pass (GPU command thread), multithreaded-rendering hang

Measurement setup (RTX 5070, Ryzen 5 5600, 1080p native, vsync and frame limit off, engine cap
120 FPS): GOW3_FRAME_TIMES (every flip interval; tools/frametimes.py: mean/p50/p95/p99/1% low),
nvidia-smi utilization, the 5 s Frame stats line; routes replayed with GOW3_PAD_REPLAY
(C:\dev\routes: U3 bar fight, generic explore route); A/B with live toggles in alternating 5 s
windows. Profiles: GOW3_PROFILE with GOW3_PROFILE_MS=5 (the default 1 ms suspension inflated waits
from ~8% to ~30%), symbolized with llvm-symbolizer. Nsight Systems needs administrator rights to
sample (its UAC prompt was not accepted twice): no Nsight trace was taken.

- **U3 bar fight was CPU-bound**: 52.7-54.2 FPS, p99 25-27.7 ms, GPU 41-46%, GPU command thread
  busy 99.9%. Profile (inclusive): pipeline lookup 12.4% -> RefreshFlatBuf 10.5% -> SyncSrtTables
  10.1% -> Rasterizer::IsMapped 5.4%, whose shared lock allocated and freed a thread_local
  unordered_map node per acquisition (~7% in lock bookkeeping); host-copy waits 13.9%; GPU
  readback waits 7%.
  - IsMapped: per-thread cache of mapped intervals, valid while mapped_generation (bumped before
    and after every map/unmap under the lock) is unchanged (toggle bit 59 off).
  - Recursive locks: held locks in a 16-entry per-thread array (toggle bit 60: map only).
  - Same session, alternating 5 s windows: 16.11 vs 16.85 ms (-4.4%), p99 19.4 vs 20.0 ms.
    SyncSrtTables 10.1% -> 3.9%, pipeline lookup 12.4% -> 6.1%. The thread now waits for the GPU
    ~30% of its time (Frame stats "GPU ticks 29.4%"): CPU writes into pages holding GPU data in
    flight (1852 per 5 s, 1561 of them beside the GPU bytes) wait for the readback
    (DownloadMemory -> Scheduler::Finish). Inherent to the readback protocol; the lazy-write
    shortcut stays off (it corrupted U2's skinning tables on 15 Oct).
- **Multithreaded rendering** (GOW3_DRAW_PIPE/GOW3_VK_RECORD_THREAD, launcher option, off by
  default): U3 bar 62.8 -> 80.0 FPS (+27%), p99 18.7 -> 14.0 ms, worst 20.6 -> 14.9 ms.
  - It hung U3 after ~10.6 minutes (bathroom after the bar): no frames, GPU command thread at
    100% CPU in std::set::find under BufferCache::WriteBackGpuData (a readback range tree with a
    cycle), a job thread waiting in a write fault. tools/hangdump (dbghelp StackWalk64 of every
    thread of a live process) found it. GOW3_READBACK_RACE=1 (owner check in every function that
    touches pending_readbacks) reported 7 overlaps in 3 minutes: the GPU command thread running a
    queued ReadMemory request while DrawRec was in CompleteReadbacks / ObtainBuffer /
    WriteThrough. Cause: Liverpool::ProcessCommands drained the draw pipe only if num_commands was
    set on entry; a command queued just after that check ran beside the recording thread. Fixed:
    drain before each command. After: 0 overlaps; 15-minute U3 soak (route replayed, game
    progressed past the hang point): no hang/crash/assert, 136521 frames, p99 12.3 ms.
  - U2 DLSS did not run with multithreaded rendering: the flat user data reaches BindBuffers as a
    constant-ring copy, so the flat-camera hook never saw it. The ring branch now feeds the camera
    search; "DLSS 960x540 -> 1920x1080" with UNCHARTED_MULTITHREADED_U2=1.
- Tests (MSYS2 CLANG64): flat-camera, scene-coord and the other host tests that build on Windows
  pass, except orb-multiplier-test and stat-multipliers-test (God of War III settings asserts in
  gow3_settings.cpp, untouched since 05e81f4; not rerun at that commit). camera-motion,
  fiber, graphics-settings, motion-shader, pipeline-binary-cache, scene-resolution, taa-shader,
  upscaler-support and videodec2 tests do not build on Windows (Linux-only stubs).
- Research (primary sources, 18 Oct): no Claude Code skill or plugin for Vulkan/C++/emulator
  profiling in the official catalogs (claude-plugins-official, anthropics/skills); useful MCP
  servers: Tracy's own MCP server (wolfpld/tracy extra/mcp, Tracy 0.14; upstream shadPS4 already
  links Tracy 0.11.1, which would need an update), RenderDoc MCPs (JiaboLi-GitHub/renderdoc-mcp),
  an ETW trace MCP (nijosmsft/etw-mcp). None was installed or tested here.
- **More checks, 18 Oct (after the fixes above):**
  - U2, multithreaded + DLSS Performance (launcher-written ini: upscaler=dlss, preset=3), explore
    route replayed 5 minutes: 0 readback-race reports, no assert, 40125 frames, p99 10.1 ms.
  - U1, multithreaded: game assert `GetRenderFrameParams(frameToFreeInstancesFor)->m_gfxEopTick`
    after ~5 minutes (2 of 2 runs); single-threaded 5 minutes fine (p99 15.9 ms). The pre-change
    build (01b17c9, worktree) asserts too, after ~30 s of play: not introduced here. Lead: U1
    defers ~1800 end-of-pipe fences per second behind ray-cast readbacks ("ray-cast fences written
    after their readback", 2.2-2.6 ms on average, up to 98 ms with multithreaded rendering); the
    game checks a frame's EOP tick two frames later. Multithreaded rendering stays off by default.
  - Vulkan validation layer (SDK 1.4.363, sync validation on), U3 bar fight replayed ~2 minutes:
    the same 5 ids in the pre-change build, current single-threaded and current multithreaded
    (VUID-vkCmdCopyImage-srcImage-01548/-srcSubresource-10211/-10212, SYNC-HAZARD-WRITE-AFTER-WRITE
    and READ-AFTER-WRITE on U3's 960x540 D32S8 half-resolution depth): nothing new.
  - Pixel check of toggles 59+60 on a paused U3 frame: inconclusive (the paused frame is not static:
    two captures with the same settings differ in 90626 pixels, max 32; on vs off 52436, max 24).
  - Package (DLSS_BRIDGE/DLSS_SDK set): extracted to an empty folder and run with its own Python:
    "DLSS: ready", "DLSS 1920x1080 -> 1920x1080, quality 0, preset 11", 60 FPS; without the two DLLs
    "DLSS unavailable (...not installed); falling back to FSR 3.1", 60 FPS, no errors. A non-NVIDIA
    GPU was not available (the integrated Radeon is no longer in the Vulkan device list).
  - U1 and U3 cameras: UNCHARTED_UPSCALER_EXPERIMENT=1 finds them with the same flat-user-data
    search (U3: vertex user data float 86, U1: float 77; depth 1 - 0.24/z as in U2). U1 jungle,
    fast pan: plain frame-to-frame 36.5% of pixels below 24/255, debug view mode 4 (reprojected)
    83.4-85.4% (residue: wind-animated foliage, the revealed screen edge). U3 not validated: the
    reached scenes (bathroom brawl) have a scripted camera (plain frames already 98.6% equal).
    Still to find per game: the HDR scene color and the first post-processing dispatch (U2's
    aec539f0) before DLSS can be enabled there.
- **U1 and U3 DLSS, 18 Oct.** GOW3_UPSCALER_TRACE=1 now prints, every 60th frame, the scene color
  and the distinct compute shaders dispatched after the frame's last scene color draw. Both games
  use U2's chain: RGBA16F 1920x1080 scene color, then cs 6a6422a9 (copies), e8746089, aec539f0
  (bloom downsample), 7095d6c2, 04f81a8f... so all three trigger before aec539f0; the probe now
  sets the upscaler environment for every game with upscaler != off (the experiment switch is
  gone) and the launcher's DLSS text no longer says U2 only.
  - U3 camera validated in the bar fight with DLSS's own motion vectors: while the camera moves,
    Laplacian sharpness of a wall crop 1.47-1.52 with the computed vectors vs 1.14-1.24 with zero
    vectors (toggle 58; the graffiti doubles). The debug overlay cannot run there (the frame before
    the display pass is R8). The camera search finds float 86 or 71 depending on the first scene
    shader of the frame.
  - U1 in scaled presets showed only characters and some foliage on a fog-colored world.
    GOW3_SCENE_DEBUG (now with guest addresses and native writes): U1 lays down its scene depth
    (1130438000) in ~94 depth-only draws before the first color draw; the scene only started at a
    draw with a color target, so the pre-pass stayed native and the reduced world draws (equal
    depth test) ran against a resampled depth and failed. flat_scene_draw no longer needs a color
    target. After it: U1 Quality correct, 142 of 207 scene passes reduced; U3 Quality and U2
    Quality unchanged (U2 still 22 of ~190, as before the change).
  - Checked at 1080p, RTX 5070: U1 DLAA (walking Nate, no trails; grass edges smoother than
    without), U1/U3/U2 Quality 1280x720 -> 1920x1080. U1 at Quality 68 FPS vs ~70-77 at DLAA: CPU
    bound at 1080p (2300 draws per frame), like U2.
  - The intermittent U1 `m_gfxEopTick` assert also hit two single-threaded runs during loading
    (34 s and 93 s; the next three runs were fine).
- **FrameBegin m_gfxEopTick assert fixed, 18 Oct (probe patch FRAME_BEGIN_WAIT_PATCH).**
  boot-linked.bin addresses: image offset = file offset - 0xb6bc8 (G3PROBE5 header); the probe's
  "image offset" lines are file offsets. U1 1.02 FrameBegin (image 0x4d2700 area) only checks
  frame N-3's params slot (0x10cdb40 + (N & 31) * 0xa50, m_gfxEopTick at +0xa08); it never waits.
  A hardware write watch (tools: watch_write.py on Uncharted.exe) showed the tick is written by
  the CPU at 0x51ea7c, in a thread function 0x51e630 (eight threads, 0x522e94) that waits in
  sceKernelWaitEqueue (1 ms timeout) for the graphics end-of-pipe event, walks the frames after the
  last done one and, for each whose done label (table 0x11a2ba0 + (N & 31) * 0x40, pointers at
  +0x18/+0x20/+0x28 into 0x1036221xxx) reads 1, stores rdtsc as the tick. GOW3_EOP_RANGE_TRACE
  (now also EOS, WRITE_DATA, compute RELEASE_MEM, held fences and the interrupt flag) showed the
  three labels per frame are all EOPs, in order clock, frame number, 1, the interrupt on the last.
  So the tick needs that guest thread to wake within the game's margin, and the margin is nil:
  sampling the table during multithreaded play, 97% of samples had three prepared frames without a
  tick (the game runs exactly at its limit). Any lag of the handler thread (loading, multithreaded
  rendering, readback-held fences) asserted. The patch turns the assert block after `cmp
  [rax+rcx+0xa08],0; jne` into `pause; jmp` back to the compare (pattern
  4883bc08??0a000000753b488b05: unique in U1, U2 at +0xa28, U3 at +0xa28): FrameBegin waits for
  the tick. Safe because the GPU command thread lands held fences by itself while idle.
  - U1 multithreaded, explore route replayed 10 minutes: no assert (63740 frames, 100.3 FPS mean,
    p99 13.4 ms); before, 2 of 2 runs asserted after ~5 minutes. U1 single-threaded: 3 more boots
    into gameplay fine. U2 and U3 multithreaded, 30 s of play each: patch applied, no assert.
- **Multithreaded rendering on by default, 18 Oct** (probe GOW3_DRAW_PIPE/GOW3_VK_RECORD_THREAD
  default 1; launcher GAME_SETTING_DEFAULTS multithreaded True, config version 7 turns it on once
  in saved configs). The user then saw U2's train flash white for single frames (opening train
  scene). Measured with tools in C:/dev/cap (frames.ps1 grabs ~21 frames/s of a window region;
  flick2.py counts pixels of luma >= 195 that are > 70 brighter than both neighbouring frames,
  calibrated: 0 single-threaded, 7-12 per 150 frames on the bad build). Bisected: draw pipe only
  (not the recording thread, not DLSS, not toggles 59/60); live toggles 38, 39 or 40 each hid it.
  Cause: a dispatch handed to the recording thread binds and marks its written buffers only
  there; the GPU command thread meanwhile prepares later draws and SyncSrtTables reads constant
  tables from guest memory, checking only ranges already marked GPU-written, so those draws used
  the previous frame's constants (cs 6a6422a9 copies the train's). Fix 1 (ffc2956): the copy
  shaders and c92150ba drain first; the user still saw rare flashes. Fix 2: DispatchDirect records
  the written buffer ranges of pipelined dispatches (desc.GetSharp at stage A), DrainDrawPipe
  clears them, and SyncSrtTables drains before a read overlapping one (toggle 1 << 61 skips).
  Draining before every walk instead cost 15% in U3's bar fight (56,000 drains/s). Results: only
  the SRT check (GOW3_DRAIN_CS=none) 0 flashes in 600 frames, same session with toggle 61: 16 in
  300; final build 0 in 1000; U3 bar fight uncapped 113-117 FPS with and without the check
  (~5,300 drains/s both); U1 multithreaded 30 s fine.

## 18 October 2026: object motion vectors in the Uncharted games; DLSS from NVIDIA

- **nvngx_dlss.dll not packaged** (user decision: players get it from NVIDIA). package_windows.sh
  ships gow3_dlss.dll (MIT bridge; it statically links NVIDIA's nvsdk_ngx_s.lib, so NVIDIA's
  license still goes with it) and refuses a stage containing nvngx_dlss.dll. Launcher: "Get from
  NVIDIA" (github.com/NVIDIA/DLSS lib/Windows_x86_64/rel) and "Add nvngx_dlss.dll..." (checks a
  valid Authenticode signature by NVIDIA Corporation, copies to gow3_pc/out); apply_update.py
  carries it into new versions (CARRY). Tested from an extracted package: without the file "DLSS
  unavailable ... falling back to FSR 3.1"; after adding it through the launcher "DLSS: ready".
- **Object motion for Uncharted** (GoW3's path, vk_object_motion: the vertex shader stores its
  clip positions per vertex, the fragment writes previous - current with its depth; the compose
  pass keeps it where that depth matches). Three gaps, all GoW3-specific selection:
  1. Passes: GoW3's 5-target G-buffer only. Now also (GOW3_CAMERA_SOURCE=flat) draws testing a
     scene-size D32 depth (pitch >= 3/4 of the native width: not the 960x540 passes, which made
     the motion image alternate sizes and Finish() twice a frame) with 1-4 unblended targets,
     with or without depth writes (U1 draws after a depth pre-pass with an equal test).
  2. Selection: by bone-palette constant buffers (GoW3 sizes); Uncharted's vertex shaders have
     none (only vertex streams: 12/8/4/28/36-byte strides) and their per-draw constants change
     every frame for static objects too (GOW3_FLAT_DIFF: dword 0 and 34/38, pointers, and a
     12-dword camera-relative matrix), so every such draw takes the motion variant, chosen
     before the stage refresh (choosing after it refreshed the stages twice: RefreshFlatBuf and
     SyncSrtTables per draw, -9% in U1, found with GOW3_PROFILE).
  3. Geometry key: from the fetch shader's vertex attributes; Uncharted pulls vertices with
     buffer loads, so the key is now the hash of the vertex shader's read buffers.
  Results (GOW3_DUMP_TRIGGER now also dumps the DLAA path: motion, output, object attachment):
  U1 jungle 77-78% of pixels with object vectors (Nate, Sully and wind-moved plants with their
  own silhouettes in the motion image), also in Quality (1280x720 attachment); U2 train 90%; U3
  bathroom 27%. 810-930k vertices of 4M per frame. U1 uncapped: ~75 FPS with, ~71-78 without
  (noise); GPU 39% (CPU-bound). GOW3_OBJECT_MOTION=0 switches it off.
- **Snow (U2) not done.** The DLSS bridge (ABI 2) now passes the reactive mask as NGX's
  bias-current-colour mask, opt-in like FSR 3's mask (reactive setting). Tried as default for
  Uncharted: U2's effects blended against its depth copy (vs e1b17168/ps ecd20bdd) are mist
  cards, and the masks marked the rock, not the flakes; skipping the three full-screen effect
  composites (ps b4fa3a82, 84adaaea, 53098288) left the flakes, so where the snow is drawn is not
  found yet. GOW3_UPSCALER_TRACE prints these events ("Scene effect", "Scene composite", the
  mask events per frame).

## 18 October 2026: CPU load (job workers, draw pipe waits)

- Process CPU at 60 FPS (vsync, cap 60) was 6.5-6.7 cores busy in U2/U3 (C:/dev/cpuuse.ps1,
  per thread: C:/dev/threads.ps1 with thread descriptions): six NdJobWorkerThread at 80-99% each,
  the GPU command, draw recording and Vulkan recording threads ~45% each. Sampling the workers
  (C:/dev/tools/sample_workers.py): the job system's idle loop (U3 1.02 0xda6470-0xda66d6)
  spins with `pause` unless the scheduler flag +0x59 is set; with it, after 1000 idle rounds it
  locks the scheduler mutex (+0x60) and waits on its condition variable (+0x80, 10 ms timeout);
  the producer (0xda589f) signals when the flag is set. SetWorkerSleep (0xd96440) stores the
  flag and the workers' priority but is not called on the way into gameplay (patching it changed
  nothing; the flag stayed 0). Writing the flag in a running U3: 6.7 -> 2.4 cores at 60 FPS,
  uncapped 111-112 FPS either way; in a running U1 it asserted in movie-player.cpp:2152 (switch
  while jobs were in flight). WORKER_SLEEP_PATCHES (probe) make the three readers act as if it
  were set from the start and lower the timeout to 1 ms (the worker does not re-check its queues
  under the mutex; a job queued in that window waits for the timeout). Same bytes in the three
  games. Uncapped, patch vs UNCHARTED_WORKER_SPIN=1: U3 4.9 vs 8.2 cores, 112-117 vs 111 FPS;
  U2 4.2 vs 7.7 cores, ~120 both (engine cap); U1 4.8 vs 8.2 cores, 104-107 vs 79-80 FPS. No
  faults in these runs.
- Draw pipe waits on the GPU command thread (profiles, GOW3_PROFILE=10 GOW3_PROFILE_MS=5): U2
  stage A waited 30%: LandDeferredFenceFor 11.4% (drained the pipe before looking up a fence held
  for its ray-cast readback) and DispatchDirect 5.8% (the copy-shader drains of ffc2956, now
  covered by the SRT range check). deferred_values is now behind a mutex and read without a
  drain when no later write to the label is in the pipe (toggle 1 << 63 restores the drain); the
  copy-shader drains are off (toggle 1 << 62). A/B in one session: no FPS change (U3 109.6 vs
  109.3, U2 at its 120 cap): these waits were not on the critical path.
- Seen once in 7 U2 starts with multithreaded rendering (13:16, not reproduced in 6 more): a
  read fault in Rasterizer::FindPreparedStage (bind_prepared pointing at a freed prepared draw)
  on the draw recording thread during loading. Not fixed.

## 18 October 2026: GPU-offload audit, register scan, constant ring reuse

- **Audit** (8 areas, 2 adversarial checks each): nothing left on the CPU can usefully move to the
  GPU. Detile, image copies/clears, indirect draws, Quad/Rect expansion and cs 6a6422a9 already
  run there; the rest (SRT walks, pipeline lookup, sharps, constant snapshots) must be on the CPU
  before the draw is recorded. Importing guest memory (VK_EXT_external_memory_host) was refuted:
  fences complete when host copies finish, not when the GPU does (holding them cost 83 -> 79 and
  60 -> 42 FPS before). The work is making the GPU command thread (stage A) cheaper.
- **RegDirty** (regs.h): 26 u64 words walked with countr_zero instead of testing 1664 bitset bits
  per draw (PostDraw self 19.2% -> 5.6% of stage A). U1 uncapped A/B: 99.3 -> 106.7 FPS.
- **Fixes from the audit:** NotePendingGpuWrite from PostDraw recorded Head() before Commit
  (entries now take the position after it); FixAimFocusCentre ran only on the non-ring path
  (now also before the ring copy); BufferCache::WriteThrough could land in the stream buffer and
  leave the arena stale (now always the arena).
- **Constant ring reuse** (toggle 1 << 56): U1 copied 2.35 GB/s (850k copies/s, ~3 per draw);
  70% repeated an (address, size) copied since the last drain. Reuse per epoch (bumped by any
  drain, any RunInOrder task, and NotePendingGpuWrite over this epoch's copied bounds); reused
  regions are held in ConstantRing until the reusing packet retires (EndPacket gives that packet a
  unique ring end). GOW3_RING_REUSE_CHECK=1: 0 mismatches in ~2.8M reuses / 5 s (U1), 0 in U2/U3.
  U1 reuses 1.46 GB/s; U3 only 40 MB/s (3350 drains/s). U1 A/B: 110.3 -> 119.1 FPS (120 cap).
  Frame stats print "constant ring N guest copies/s, X MB/s" and the reuse line.
- Tried and dropped: lock-free granule filter for IsRangeGpuWrittenShared plus an SrtPlan memo:
  no measurable change with GOW3_FPS_LIMIT=0 (U1 113.9 vs 117.1, U3 141.9 vs 139.9, scene noise).
- U2 train regression: 0 white flashes in 1000 frames (new C:\dev\cap\frames2.ps1 uses
  PrintWindow; CopyFromScreen captured whatever window was in front).
- **FIXED (18 Oct): cutscenes too fast above 60 FPS, all three games.**
  - Measured by camera-cut times (C:\dev\cap\cuts.ps1 thumbnails, cuts.py/cutfit.py vs 60 FPS, 9 cuts,
    rms 6-15 ms), U3 opening: 30 FPS 1.000x, 45 1.000x, 90 1.501x, 120 1.996x: speed max(1, fps/60).
  - The cutscenes are pre-rendered movies (build/main/movie1/ps4/*.bsf: U1 40, U2 48, U3 38) played
    by ndlib movie-player.cpp. Its frame step (U1 0x4a0e62, U2 0xb84805, U3 0xdefd55) does
    counter[+0x2254c]++ every game frame while the game clock runs, then catches up with the audio
    clock target [+0x22550] when behind (up to 3 more); never waits when ahead. One game frame per
    vblank on the PS4, so it was fps/60 too fast here; the movie audio stays real time (Ajm decode
    rate identical at 60 and 120 FPS).
  - Found with GOW3_FPS_LIMIT_FILE (30 -> 120 FPS in one run) and memrate5.py: ints growing 2 per frame
    at 30 FPS and 1 per frame at 120, written by the movie player (watch: 0xdee5b9, 0xdefd5d).
    Ruled out on the way: master/sub clocks, anim manager (sub0 dt, drift 0), the 10 rodata 1/60
    constants (set to 1/240: still 2.0x), periods counter (+0x14 = 2.5 at 30 FPS: still 1.0x),
    VideoOut status calls (the game makes none; flips go through Gnm and flip events).
  - Fix: src/movie_hook.h, installed by probe.c for any build whose code matches (outside the
    --no-game-hooks gate): a call to a stub that accumulates the clock's dt and shows the next frame
    from half a vblank owed, paying one vblank, at most one per game frame. At or below 60 FPS
    (frame-limited, phase noise under +-4 ms) every frame still advances; audio catch-up unchanged.
    GOW3_MOVIE_PACING=0 keeps the original step. tests/test_movie_hook.cpp: 600 frames in 10 s at
    60-240 FPS, signatures found once in each game. U3 at 120 FPS with the hook: 1.001x (9/9 cuts).
    U1/U2: hook installs (boot.log "Movies: frame pacing"); their cutscenes were not measured.
    The stub page is allocated within +-2 GiB of the hook (probe.c allocate_near, VirtualQuery walk):
    a page below 2 GiB only reached a --low-image guest, and a packaged U3 boot printed "frame pacing
    unavailable (hook outside relative-call range)". The cheat, health bar and red orb stubs use it too.
  - New diagnostics: GOW3_FPS_LIMIT_FILE (frame limit from a file, read twice a second);
    GOW3_EXEC_WATCH_FILE takes up to 4 addresses, r: prefix for read/write watches;
    GOW3_EXEC_WATCH_UNIQUE=1 logs each instruction once and prints per-instruction counts when the
    watches move; GOW3_AUTO_CHOICE=1 accepts the save dialog's default (unattended new-game tests).

## 19 October 2026: cutscenes at ~10 FPS at 4K (Lanczos movie scale)
- User: U3's first cutscene at ~10 FPS (package 0.4.0, US 1.02, 4K, DLAA, MT on, RTX 5070, 12
  threads). Log: movie frames 75-113 ms (9.5-15.9 FPS), GPU thread idle 97%, "Movie: 1920x1080 ->
  3840x2160 scaled with Lanczos, 17.3 ms a frame". Reproduced with play.py (title, no input, the
  attract cutscene): 9.2-9.4 FPS. GOW3_MOVIE_SCALER=bicubic in the same build: 6.4 ms a frame, ~60
  FPS. Cause: a914707 (18 Oct) made the 4K scale Lanczos + SWS_ACCURATE_RND on the decoder thread.
- Fix: videodec2 WriteMovieCopy uses sws_alloc_context + sws_scale_frame (swscale 10 dynamic mode)
  with threads = 4; same filter. After: Lanczos 6.4 ms a frame, cutscene at the 120 FPS cap, picture
  checked (shot). Not verified: U1/U2 cutscenes, other render sizes (ultrawide bars path), CPUs with
  fewer cores. tests/test_videodec2 does not build (core/memory.h not on its include path); it did
  not build before this change either.
- Gameplay in the same session: 73-76 FPS at 4K DLAA, GPU-bound (the GPU command thread waits for
  GPU ticks 54% of the time); the user's display is 2560x1440, so a 1440p render or DLSS Quality is
  the lever there, not a code change.
- Cutscene paths in all three games (user question, same day):
  - All three executables import only libSceVideodec2 (analysis.json): every movie goes through
    gpu/shim/videodec2 and WriteMovieCopy, with the same filter and the same GOW3_MOVIE_SIZE
    condition (render size not 1920x1080). src/runtime_videodec.c (libSceVideodec v1) and
    shadPS4's AvPlayer are not imported by any of them.
  - Movies in the US dump (.bsf, 1080p): u1data/build/main/movie1 40, u2data 48, u3data 38, plus
    shareddata/movies/sce-ndi-logos.bsf.
  - Some scenes are drawn by the engine instead (U2's train opening: no MovieDecodeThread, scene
    draws): those go through the anti-aliasing/upscaler like gameplay.
  - Checked with play.py at 4K, MT on: U1 opening movie "1920x1080 -> 2880x1620 scaled with
    Lanczos, 4.0 ms a frame" (U1 renders at most 1620p), 120 FPS, picture right; U3 attract movie
    6.4 ms, 120 FPS. U2: no movie reached (title has no attract movie; the opening is in-engine);
    its movies use the same code path.

## 19 October 2026: 15.8 GB texture from a garbage T# (upstream #57, device lost)
- Report (upstream #57, 0.3.1, U1 1.02 Europe, 1080p DLSS, 100 FPS, MT off):
  "Device lost during submit" after 26 minutes. The log's last lines:
  - "Slow image create: ... image 960x540x1 mips 1 layers 6755 R8G8B8A8Unorm, 15832.0 MiB in a
    dedicated allocation";
  - texture cache memory pressure "21319 of 6275 MiB";
  - "GPU wait: 10 seconds", then device lost.
- Cause, from the log and the code:
  - Only a T# reaches 6755 layers: its depth field has 13 bits. CB/DB slice counts have 11, so at
    most 2048.
  - T#s pass the rasterizer's PlausibleImage check (guest_size < 1 GB, fully mapped).
  - ImageInfo::UpdateSize multiplied a mip's size by the layers in u32. A 1024x640 macro-tiled
    RGBA8 slice times 6755 is 16.5 GiB, which wrapped to 0.49 GiB, so the check passed.
  - The host image (Vulkan's own layout, 2.34 MiB a layer) was then 15.8 GB.
- Fix: texture_cache/guest_size.h: SaturatingMipSize/SaturatingAdd, used by UpdateSize, keep the
  size at u32 max instead of wrapping. The T# is then rejected and logged as before ("Ignoring a
  garbage T# ...").
  - tests/test_image_size.cpp (image-size-test) checks the arithmetic, including the old wrap of
    this descriptor (static_asserts).
- Not covered:
  - The RT constructors (ColorBuffer/DepthBuffer: slice size times layers, u32). Their 11-bit
    slice count keeps them under 4 GiB for render-size targets.
  - Where the garbage T# comes from: the log names no shader. It was not reproduced; the
    reporter played 26 minutes.

## 19 October 2026: crash triage for 0.5 (upstream issues, the fork's own failures)
The fork has no issue tracker (issues are disabled on GitHub), so its own failures come from
the soak runs below and from the player's logs. The upstream reports were filed against
0.2.x-0.3.1, not the fork. Each conclusion names its source.

Method:
- `gh` downloaded each issue with its attachments (boot.log, launcher-last-run.log).
- Signature: the crash block (fault type, guest offset or host module) plus the caller chain.
- Offsets were read in the 1.02 executables, the same in US and EU, with llvm-objdump of
  analysis/<game>/eboot.elf. The instruction at each offset starts a decoded instruction, so the
  reports were on 1.02.
- Versions come from the issue text, or from the report date against upstream's release times.

| Issue | Game | Port | Signature | State | Source of the conclusion |
|---|---|---|---|---|---|
| #57 | U1 | 0.3.1 | Device lost after a 15.8 GB image (garbage T#, 6755 layers) | **Fixed in the fork** (d3b1e13) | log + code (u32 wrap), image-size-test |
| #59, #60 | U1 | 0.3.1 | read 0 at 0x5f64f3 (`mov (%r13),%r8`, r13=0) in render setup fn 0x5f4ea0; callers 0x56786e <- 0x58adda | **Not reproduced** | see below |
| #42, #30, #34, #24 (U1 logs 06/09) | U1 | 0.2.3-0.3.0 | call through null at 0x64011f / read at 0x63f61e (material constant setup fn near 0x63f..0x641, called from 0x501ea9) | **Not reproduced; same family as below** | log + disassembly |
| #24 (U2 logs 07/11) | U2 | 0.2.x | call through null at 0xd667ff / read at 0xd65cfe; same register pattern (rcx=0x3b0) | same family | log |
| #54, #24 (U3 logs 08/13) | U3 | 0.2.x-0.3.0 | read at 0x12d83fe / 0x12d83b0 (callers 0x12da312 <- 0xe63ef9); same register pattern | same family | log |
| #58, #48, #43 (#29 likely) | U1, U3 | 0.3.0-0.3.1 | stop at boot: `open(/app0/uNdata/build/main/bin/shaders/src/files.json) -> not found`, ASSERTION root (ingame-shaders.cpp) | **Needs information** | log; does not happen with the fork's US 1.02 dump |
| #47 (first log) | U3 | 0.3.0 | host C++ exception 0x20474343 on the GPU command thread | **Needs a log from 0.3.1 or later**, which names the error | log + upstream 0.3.1 changelog |
| #47 (second log) | U3 | 0.3.0 | "Invalid packet ... Unreachable code" with multithreading | **Already fixed in the fork** (liverpool.cpp BehindHeldFence, 19 Oct) | same message as the fork's own repro |
| #56 | U2 | 0.3.1 (by date) | host fault at Uncharted.exe+0x3050f8 (Radeon 780M), 35 FPS, 492 ms frames | **Needs information**: the exact build to symbolize, or a log from the fork | log |
| #24 (U2 log 03) | U2 | 0.2.x | ASSERTION m_lookAt NaN (player-base.cpp) | **Already fixed** (0.2.3 focus reset; the fork has it) | upstream changelog |
| #24 (U1 log 01, U3 log 05) | U1, U3 | 0.2.x | read 0x3f0 at 0x581ade; read 0x34 at 0x849ddc | **Not reproduced**; single reports from 0.2.x | log |
| #41, #33, #27 | U2, U1 | 0.3.0-0.3.1 | no boot.log attached (#41 boot.json only, #33 upload failed) | **Needs information** | issue text |

Details:
- #59/#60 (the newest U1 reports, both 0.3.1). #59 says chapter 3 "A Surprising Find" at the
  plane, crashing even standing still, on an i5-8400 at 1440p/1080p; #60 is an MX130
  at 960x600, 30 FPS.
  - Reproduction tried here at chapter 3's start, with camera and walking: no crash in any of
    these runs.

    | Run | Settings | Time |
    |---|---|---|
    | 1 | fork build, 1440p, MT on | 3 min |
    | 2 | MT off, GOW3_COPY64_EOP_READBACK=0 (as 0.3.1: no #6 readback) | 1 min |
    | 3 | soak, 4K DLAA M, 120 FPS cap, MT on, camera and walking every 20 s; stopped early at the user's request, U2/U3 not run yet | 6 min (16:02-16:08), 95-106 FPS |
  - The crashing register comes from a local (-0x120(%rbp), saved at -0x150) loaded early in
    the 7 KB function, which writes PM4 packets.
  - Both reporters' PCs are far slower than this one (Ryzen 9 / RTX 5070), so a timing race is
    likely. What would settle it: a fork build's log from those PCs.
- The 0x64011f / 0xd667ff / 0x12d83fe family is one engine routine in all three games:
  - It walks a material's parameter table and reads resource pointers.
  - It calls a setup function per parameter (`callq *%rcx`); a null pointer means the table was
    overwritten.
  - That is the "function pointer turned to 0" case that upstream 0.3.0's readback guard was
    for. #42 (stated 0.3.0) and #54 (0.3.0 by date) still crashed there after it.
  - The fork adds the held-fence ordering (BehindHeldFence), the flip and readback fixes of
    19 Oct, and the U1 copy-shader readback (#6). Whether that is enough is not proven: there
    is no repro, so the soak runs are the check.
- files.json stop (#58, #48, #43):
  - The game opens the shader list loose, after its archive lookup fails. That points at the
    update's data.psarc not being the 1.02 one in the combined view.
  - All the reporters use repacks (AnkerGames, AUCTOR.TV). #43 matched upstream's bin.psarc
    hashes but not data.psarc's.
  - Known-good US 1.02 data.psarc files (this PC, plays):

    | File | Size | SHA-256 |
    |---|---|---|
    | CUSA02320-patch/u1data/data.psarc | 12,194,412 | a979389b847c60aae55e4255ec729c71bab3c3cbd6d61ed9e4a5ffa23e63344f |
    | CUSA02320-patch/u2data/data.psarc | 13,898,047 | 6bcba1c4e4962262be9a8a2334330f1ce052952886c5c7f99efc83ca0ccdd3b2 |
    | CUSA02320-patch/u3data/data.psarc | 15,818,236 | fee6e5e4e00807d2e01cd0b074373eb4357723c888ac3d9b3c0703ba732b1fad |

    Base 1.00 sizes: 12,169,836 / 13,866,452 / 15,775,188.
  - What is needed: the reporters' sizes and hashes of those three files, and a listing of
    analysis/views/game-update-*.
- Tooling note: in U1's Chapters menu, the "Friend Stats are not available when offline" box
  ignored `play.py press cross` (12 times). `play.py seq "cross@150" "@1500"` (press, then
  release) closes it, as a real controller does (the user, 19 Oct). It is not a game or port
  bug.
- Soak note: the soak script was not stopped with U1's game. It went on to U2 and ran it from
  16:08 to 16:17 (about 9 minutes, Continue, the camera and Nate moving): no fault, 69-70 FPS.
  U3's 30-minute soak and U1's and U2's full 30 minutes are still to do.

## 19 October 2026: Uncharted 3 performance at the player's settings

Setup:
- Player's settings: 4K, DLAA (DLSS model M), 120 FPS cap, multithreading on; RTX 5070, Ryzen 5
  5600.
- Scene: the US save's Continue, the bar fight, standing still.
- Tools:
  - PresentMon 2.6 (winget; the account is in Performance Log Users) runs inside `play.py
    measure`, which now prints PresentMon's frame times, time on screen, GPU busy and GPU wait.
  - `C:\dev\bench\ab2.sh` alternates on/off windows in one session, the order flipped every
    round, and keeps each window's raw frame times.
  - `abstats.py` gives a hierarchical bootstrap (windows resampled within each state): median,
    p99 and 1% low, with 95% intervals of the difference.
  - The port's sampling profiler (GOW3_PROFILE=10, GOW3_PROFILE_MS=5) on the GPU command, draw
    recording, Vulkan recording and game flip threads, symbolized with llvm-symbolizer. The build
    is RelWithDebInfo with DWARF; LTO is unavailable in this toolchain.
  - Not available: WPR/ETW kernel sampling ("Failed to enable the policy to profile system
    performance": it needs an elevated prompt); Nsight (not installed); scipy (the bootstrap is
    numpy).
- Noise, A/A (6+6 windows of 10 s, model M): median +0.01 ms [-0.02, +0.03], FPS +0.08
  [-0.19, +0.33]. p99 moves by up to 1-2 ms between windows of the same build.

What limits the frame:
- At the player's settings the GPU is the limit: PresentMon GPU busy 14.6 of 14.8 ms (99%).
- GOW3_GPU_PROFILE, 15.6 ms of GPU work per frame:
  - DLSS evaluate (model M, DLAA) 7.9 ms;
  - GPU idle between submissions 0.8 ms;
  - the port's passes (camera motion vectors 0.23, merge 0.17, a 4K R8Uint upload 0.07);
  - the rest is the game's own passes.
- DLSS cost is NVIDIA's, not the port's. NVIDIA's DLSS Programming Guide (execution times,
  RTX 5070, 3840x2160 output) lists K 2.11 ms and M 2.98 ms. The port measures, in Performance
  mode (1080p input): K 1.99, M 2.90. With DLAA (4K input) it measures K 2.2, M 7.9. So model M
  scales with the input size and model K hardly does. NVIDIA's defaults are K for DLAA, Quality
  and Balanced and M for Performance.
- With model K at DLAA, U3 is CPU-bound at ~97 FPS (GPU busy 86%):
  - The game's submitting thread waits 61% of the time in WaitGpuIdle. After sceGnmSubmitDone,
    the next frame's first submission waits until the GPU command thread has finished the
    previous frame (GOW3_SUBMIT_WAIT_STATS: ~97 waits a second, 6.4 ms each).
  - The GPU command thread (stage A):
    - compute dispatches 48%: U3 has ~600 dispatches of one 1x1x1 group a frame, plus 74 and 60
      of two others;
    - constant ring copies (memcpy in CopySparseMemory) 24%, 2.5 GB/s;
    - draw pipe drains 24%;
    - compute pipeline key refresh (RefreshFlatBuf through SyncSrtTables) 15%;
    - LandDeferredFenceFor 11%.
  - The draw recording thread (stage B) is 72% busy: dispatch bindings 28%, GpuRangesAdd and
    its interval set 12%.

Constant ring reuse (toggle second word 32 RingPreciseWrites, 64 RingKeepOnCommands):
- Before: 2.5 GB/s of guest constants copied, 55% of the bytes repeating a range already copied
  in the same flush (GOW3_RING_STATS), but only ~40 MB/s reused. A new epoch (no reuse of
  earlier copies) started:
  - on every in-order task: ~97,000 a second, the EOP/EOS events, WRITE_DATA, DMA and compute
    RELEASE_MEM around the dispatches;
  - on every drain, ~3,300 a second of them for queued commands (guest fault downloads);
  - on any GPU write between the lowest and highest copied address, which covered nearly every
    write.
- Now:
  - A GPU write starts an epoch only over a 1 KiB line a copy of the epoch came from (hashed
    epoch tags): ~290,000 writes a second keep it, ~1,100 do not.
  - WRITE_DATA and DMA already note their exact range. The events and RELEASE_MEM note their
    label (8 bytes).
  - Command drains keep the epoch: they write only GPU-written memory, which is never copied.
  - New: an epoch per guest submission and per compute ring ding (NewGuestEpoch), and after
    every WAIT_REG_MEM. The guest may rewrite constants once a label tells it the GPU is done;
    the old rules covered that by accident.
- After: 1.7 GB/s copied, 820 MB/s reused; the ring is no longer full (it handed 1-3 thousand
  copies a second to stage B before, 0 now).
- Correctness:
  - GOW3_RING_REUSE_CHECK=1 compares every reused copy with guest memory. A reuse only happens on
    ranges with no GPU write registered or pending, so guest memory is then the truth. Result: 0
    of ~2.8 million checked reuses differed.
  - Paused frame, toggles alternated: on/off differs in 0.29-0.56% of pixels, on/on and off/off
    in 0.13-0.69%, and on1/off2 in 0.00% (max 6/255).
  - No ASSERT/Fault/Invalid packet/VK_ERROR in the logs.
- A/B (6+6 windows of 10 s; port frame times, then PresentMon):

  | Settings | Median frame time | FPS | p99 |
  |---|---|---|---|
  | DLAA, model M (player's) | -0.25 ms [-0.28, -0.20]; PM -0.26 [-0.29, -0.24] | 67.5 -> 68.4 (+1.4%); PM +1.14 [+0.99, +1.28] | no change beyond noise |
  | DLAA, model K | -0.53 ms [-0.79, -0.15]; PM -0.69 [-1.03, -0.34] | 97.1 -> 101.5; PM 96.4 -> 102.8 (+6.6%) | -0.6 / -1.0 ms, intervals include 0 |

- Even GPU-bound, the frame is shorter: the GPU idles between a frame's last submission and the
  next frame's first, and that gap follows the GPU command thread.
- U1 (4+4 windows) and U2 boot and play. U1's scene changed during the run (99 -> 83 -> 99
  FPS), so its FPS interval is ±10; reuse there was 240-360 MB/s on and 137-291 off.
- Tests: image-size-test and movie-hook-test pass.

Model M faster (the user wants to keep model M; asked for +20%):
- NGX's time cannot be cut from the port; it follows the input size. A new mode, Ultra Quality
  (preset 5, listed second; saved preset numbers keep their meaning), draws 1/1.4 of the output
  per axis (2742x1542 for 4K). DLSS maps it to NGX's Quality mode, whose input range is 1080p to
  4K.
- Measured against DLAA, model M, U3 bar fight. Each row is its own session, 4-5 alternating
  windows, live preset switch (GOW3_PRESET_FILE, C:\dev\bench\abpreset.sh), PresentMon:

  | Mode (input for 4K) | DLSS M | FPS (DLAA 68.6-69.0) | Median frame time |
  |---|---|---|---|
  | 1.2x (3200x1800) | 6.0 ms | 74.9 (+8.7%) | -1.20 ms [-1.22, -1.18] |
  | 1.3x (2954x1662) | 5.3 ms | 79.5 (+15.9%) | -2.08 ms [-2.10, -2.04] |
  | 1.4x (2742x1542), Ultra Quality | — | 85.2 (+24.1%) | -2.95 ms [-2.98, -2.92]; p99 -2.6 [-4.2, -1.1] |
  | Quality, 1.5x (2560x1440) | — | 89.9 (+30.8%) | -3.66 ms [-3.70, -3.63] |

  The first 1.3x session is not used: the fight moved on halfway (both states went up to
  107-120 FPS).
- Checked: the default build gives "DLSS 2742x1542 -> 3840x2160, quality 1, preset 13" (85.0
  FPS); FSR 3.1 in the mode makes a 2742x1542 -> 3840x2160 context (103.9 FPS). Pictures right
  in both (960-wide live view), no errors in the logs. FSR 4 maps the mode to its Quality model;
  FSR 4 and TAA in the mode were not run.
- The launcher's and the menu's DLSS model text said M "costs a little more". It now gives the
  DLAA and Performance costs and NVIDIA's defaults.

Ruled out (do not reinvestigate without new data):
- The port's DLSS call: it matches NVIDIA's published cost in Performance mode (above).
- Removing the submission wait (GOW3_SUBMIT_NO_WAIT=1, measurement only): slower and broken.
  Model K went 89.8 -> 81.8 FPS (GPU busy 86 -> 67%); model M fell to 55.7 FPS, with visible
  glitches (the user saw them). The serialization is not the gain.
- The 4K R8Uint image upload each frame: 0.07 ms of GPU, under the noise.

Not verified:
- Other U3 scenes. The EU chapter 3 save is copied to `C:\dev\bench\backup_ch3` for that
  (BACKUP=... bench.sh).
- Other GPUs and CPUs.
- Long sessions with the change.
- U1's gain (its scene drifted).

## 20 October 2026: model M's own cost (same model, same input and output)

The user asked for a cheaper model M at the same sizes: 4K DLAA, no model or resolution change.

Tool: `dlss-bench` (gow3_pc/tools/dlss_bench.cpp, CMake target `dlss-bench`). It loads the
bridge and nvngx_dlss.dll (310.9.16, the player's) from out/, creates the feature like
RecordDlss (MVLowRes, inverted depth, HDR + auto-exposure, preset by name) and times Evaluate
with GPU timestamps. One configuration takes 2-9 s instead of a game boot (1-2 min). Options:
`--in/--out WxH --model jklm --hdr --bias --color rgba16f|r11g11b10|rgba8 --queue compute
--load N --game-images 1 --frames N`. It is linked statically and runs from any shell
(PowerShell: `& gow3_pc\out\gpu\dlss-bench.exe --model m`). Interleaved
sweeps: run each variant alternately, order flipped every round.

Measurements (RTX 5070, 3840x2160 -> 3840x2160, model M):
- Short runs (150 frames): 7.51 ms; model K 1.99 ms.
- Sustained (1000 frames): 7.90 ms median. The GPU reaches its 250 W limit and the clock drops
  2940 -> 2850 MHz (nvidia-smi). In U3 (GOW3_GPU_PROFILE, bar fight, 342 frames) "DLSS evaluate
  (NGX)" is 7.87 ms at ~210 W board power. So the game adds nothing to NGX's time: the earlier
  7.9 vs 7.5 difference is the clock under sustained load.
- Variants, each against the default in the same sweep:

  | Variant | Evaluate | Change |
  |---|---|---|
  | Default (HDR, auto-exposure, bias mask, RGBA16F colour) | 7.51 ms | — |
  | No bias mask | 7.50 | none |
  | R11G11B10 colour | 7.51 | none |
  | Colour and depth with the texture cache's flags (mutable format, extended usage, storage) | 7.51 | none |
  | DLSS on the async compute queue, alone | 7.48-7.51 | none |
  | 4.2 ms of graphics-queue load at the same time, DLSS on async compute | frame 11.66 vs 11.74 in series | -0.7%: DLSS M fills the GPU |
  | LDR input (no IsHDR / auto-exposure), sustained 600 frames | 7.43 vs 7.72 | -0.29 ms (-3.8%) |

- NVIDIA's guide: auto-exposure costs ~0.02 ms, and preset L (M in practice) ignores an exposure
  texture. So the LDR saving is NGX's own HDR handling.

What this leaves: the LDR path is the only saving, at most 0.29 ms (~2% of a 14.6 ms frame).
U3's scene colour is HDR (RGBA16F), so LDR input needs a reversible tonemap before DLSS and its
inverse after. The inverse could ride in the merge pass, which already reads the output. The
forward pass would cost a 4K read and write, ~0.1-0.2 ms judging by the 0.24 ms motion-vector
pass. That leaves ~0.1-0.2 ms (~1%) net, with a quality risk in highlights. Not implemented.

Ruled out (do not reinvestigate without new data): the port's integration (barriers, image flags,
formats, bias mask), async compute overlap, and the in-game vs standalone gap (clock under power
limit).

Not verified: other GPUs (the power limit decides the sustained clock), other DLSS DLL versions,
and the LDR path's picture.

## 20 October 2026: freezes before cutscenes and with multithreading (U3), upstream's fixes

User: "travamentos logo antes de uma cutscene" and freezes with multithreading in U3; check
whether BilalReloaded's fixes apply.

Upstream's fixes and this fork:
- #52 (freezes of 1.6 s, up to 33 s under Proton, before cutscenes): every unmap rebuilt the whole
  neighbouring view (carve_host, ~23 ms each). Upstream 0.4.0 maps guest ranges as views of at most
  2 MiB. Merged here on 19 Oct at 15:31 (4ecc8fd), unchanged.
- #51 (U3 permanent freeze with MT): compute RELEASE_MEM label written by the Vulkan recording
  thread into a protected page. The fork already had the same fix (labels always through the
  backing view) before the merge. Upstream's other step, MT off for U3, was not taken.
- Upstream since 0.4.0 (0.4.1, kuraeow's job-sleep patch for another 1.02 build): nothing about
  these freezes.

The player's own logs (Uncharted\analysis\big3-ps4_Shipping):
- The installed Uncharted.exe is from 19 Oct 14:05, before the 0.4.0 merge (15:31). So it does
  not have #52's fix.
- 19 Oct 15:17 session (attempts/boot-1792433849725852700): chapter 1 start, ~200 "mapped direct
  memory" (1 MiB each) lines, then "Videodec2 FFmpeg decoder created" and a 1476.7 ms frame.
- 20 Oct 08:42 session: gameplay at 120 FPS, 204 maps, then the movie and a 2069.0 ms frame.
- Both match #52: the map/unmap burst the game makes before a movie.

Current build, same cutscene (New Game, chapter 1 intro movie, player's settings, MT on):
- "memory map time: 65 maps 17.8 ms, 63 unmaps 2.2 ms; Windows carves 129 (2.3 ms)".
- Worst frame at the movie start 109.9 ms (was 1476.7 ms on the installed build).
- Stall profile (GOW3_PROFILE_STALLS=1) of that frame: the GPU command, draw recording and Vulkan
  recording threads idle; the game's flip thread in a guest cond_wait. The game waits for its own
  movie start (decoder thread, first frames), not for the port's GPU work. Not reduced.

Trigger files on the GPU threads (fixed):
- Stall profiles in the bar fight and museum (current build, MT on, 4K) caught one
  std::filesystem::exists of a missing trigger file taking 25-50 ms: readback.trigger on the GPU
  command thread (OnSubmit, 50 of 62 samples in a 73 ms frame; 31 of 40 in a 65 ms frame) and
  dma.trigger on the draw recording thread (25 of 31 in a 53 ms frame).
- The launcher sets these for every player (diagnostics on): capture.trigger (exists on every
  flip), dumps/trigger (a remove() on every presented frame), readback.trigger (every
  submission), dma.trigger (every 500 ms).
- gpu/shim/gow3_trigger_files.cpp: one thread polls them every 250 ms; the GPU threads read an
  atomic flag. Test: trigger-files-test. In game: dma.trigger ("pointers 256") and
  readback.trigger ("range clear") were taken and logged within a second.
- Not verified: how much this removes without the profiler. The profiler suspends the sampled
  threads every millisecond, which may lengthen such waits.

Seen, not fixed:
- One 880 ms frame during a level load: the GPU command thread waited 548 ms for the stdout lock
  in a printf (stdout is unbuffered, probe.c). The holder was not sampled. It may be the profiler
  suspending a thread that held the lock.
- "Slow submit: 107.7 / 225.1 ms in the driver" during level loads (vkQueueSubmit in nvoglv64).
- New shaders: the GPU command thread waits for the shader's capture (QueueShader
  capture_job.Wait, 24 of 37 samples in a 64 ms frame) even with async compiles.
- The hang watchdog fired once (hang-20261020-101453.txt) during a 46.7 s level load with
  UNCHARTED_HANG_SECONDS=20: a load, not a freeze.

Multithreading soak (current build with the trigger watcher): U3 chapter 3 Cartagena street
(bench save), 4K, DLAA model M, 120 FPS cap, MT on, hang watchdog at 20 s; 15 min of scripted
walking, looking and Cross (57 loops). No freeze, no hang report, no assert; lowest 5 s window
48.2 FPS, worst frame 99.5 ms. The user's MT freezes were not reproduced. The installed build
(19 Oct 14:05) is older than this one, so it is not known whether they happen on this build.

## 20 October 2026: repo audit fixes

Four fixes from an audit of the working tree. None changes the picture or the frame rate.

1. Upscaler fallback saved over the player's choice (gpu/shim/gow3_settings.cpp,
   SyncUpscalerFallback, called from ConfigureUpscalerSupport and ConfigureDlssSupport).
   - Cause: when DLSS is missing (no nvngx_dlss.dll, or a driver without it) or FSR 4 is not
     supported by the GPU, the runtime switches to FSR 3.1 for that launch. Closing the in-game menu
     then ran Save(), which wrote upscaler=fsr3 into the ini.
   - Failure case: a player with DLSS chosen starts once without the DLL; the next launch (DLL back)
     still has FSR 3.1.
   - Now the fallback also updates the snapshot Save() compares against (synced), so Save() keeps the file's upscaler value unless the player changes the upscaler in the menu.
2. Settings files written in place (gow3_settings.cpp Save(); launcher write_text_safely and
   write_config in launcher/uncharted_launcher.pyw; the upscaler=off write in
   tools/probe_uncharted.py run_game).
   - Cause: the file was opened for writing (truncated) and then filled. A kill or crash in
     between, or the launcher and the in-game menu writing at once, left an empty or partial
     probe-uN.ini, and the settings read back as defaults.
   - Now: write a temp file, then replace (MoveFileExW with MOVEFILE_REPLACE_EXISTING in C++,
     os.replace in Python). If Windows refuses the swap, the old in-place write is the fallback
     ("Settings: cannot replace ...; writing in place" in the log).
3. Kernel service thread lost wake-up (gpu/shim/gow3gpu.cpp, KernelServiceThread).
   - Cause: asio_requests = 0 ran after io_context.run() returned. A timer request that arrived
     between the return and the reset was cleared, so the equeue timer never fired.
   - Failure case: a game thread waiting for that timer hangs. The reset now runs before run().
   - Not shown that Uncharted hits this. Not reproduced.
4. Update folder remembered for another title (tools/probe_uncharted.py, run_once).
   - Cause: the launcher keeps the update folder (US 1.02) after the player switches to the EU
     CUSA02344 copy. Applying it raised ValueError and the start ended with a traceback.
   - Now the error is caught: the game starts without the update and logs "Update not used (...)".

Verified: build ok.
graphics-settings-test: PASS, with a new case (ini upscaler=dlss, DLSS unavailable, another key changed in the menu, Save() keeps upscaler=dlss and leaves no .tmp).
That test no longer built (PresentLimit's signature, FPS limit and restart rules changed since the God of War 3 import); its stale asserts were updated to the current rules in a separate commit.

Not verified: fix 3 and fix 4 were not exercised at run time (3 has no repro; 4 needs a mismatched
update folder in the ini). Fixes 1 and 2 were not checked in a game session either.

### Fixes 5-8

5. Profiler deadlock (gpu/shim/gow3_profiler.cpp, sampler loop in Start, GOW3_PROFILE_STALLS=1).
   - Cause: in stall mode the sampler pushed into the `recent` std::deque (allocates, frees blocks)
     while the sampled GPU thread was suspended.
   - Failure case: the suspended thread held the CRT heap lock; the sampler waited for the same
     lock and the game froze for good.
   - Now the stack walk stays inside the suspend window, and the deque is filled after
     ResumeThread. Diagnostic mode only.
6. Dead test (gow3_pc/tests/test_save_backup.py, deleted).
   - It imported save_backup, removed in 2872d75, so it failed on import. No behaviour change.
7. Retexture trigger on the GPU thread (gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp,
   Rasterizer::OnSubmit, GOW3_RETEXTURE_FILE).
   - Cause: std::filesystem::exists on every GPU submission, the 25-50 ms stall class of d026d97.
   - Now Gow3TriggerFiles::Watch/Take: the trigger-file thread polls it. Only when the variable is
     set.
8. Updater rollback (tools/apply_update.py apply; launcher/updater.py cleanup).
   - Cause: in the failure path, one retry(remove_item) that raised after 15 s (antivirus lock on a
     new item) aborted the handler before the restore loop ran.
   - Failure case: the old items stayed in update\old, and the next launcher start deleted that
     folder (cleanup), losing the installed version and the player's nvngx_dlss.dll.
   - Now each remove and each restore has its own try. Items not restored are listed in
     update\old\RESTORE-INCOMPLETE, and cleanup skips update\old while that file exists.
   - Known limit: the next update start still clears update\old (rmtree at the top of apply()).

Verified: build ok. gow3_pc/tests/test_apply_update.py passes (2 tests: rollback with a locked item
restores the other item and writes the marker; cleanup keeps update\old only with the marker).

Not verified: a profiler run with GOW3_PROFILE_STALLS (the deadlock is timing-dependent, not
reproduced), the retexture trigger in game, a real failed update on a live install.

### Fixes 9-20

9. DLSS signature check (launcher/uncharted_launcher.pyw ngx_signature_ok).
   - Cause: the DLL path sat inside the PowerShell -Command text between single quotes.
   - Failure case: a folder with a ' (C:\Users\D'Angelo\...) broke parsing, so a genuine NVIDIA DLL
     was called unsigned; a crafted folder name could run commands.
   - Now the path goes in the NGX_DLL environment variable; the script reads -LiteralPath $env:NGX_DLL.
10. Video decoder (gpu/shim/videodec2/videodec2_impl.cpp, VdecDecoder::ConvertNV12Frame).
   - Cause: m_sws_context was built once (sws_getContext when null) and never checked against the
     frame. av_frame_get_buffer's result was ignored.
   - Failure case: a decoder reused for a movie of another size or format converted with the old
     plan and overran the new frame buffer.
   - Now sws_getCachedContext every frame (same context while the parameters match); a failed
     buffer or context frees the frame and returns nullptr.
11. Settings Save() (gpu/shim/gow3_settings.cpp).
   - Cause: the present thread (restart prompt) and the window thread (menu close) can both call
     Save(); unlocked they raced on `synced` and wrote the same .tmp.
   - Now one static std::mutex held for the whole Save().
12. File paths (src/runtime_file.c, resolve).
   - Cause: the ".." component check accepted only "/" around it; Windows also takes "\".
   - Failure case: a guest path like /data/..\..\x reached host files outside the user folder.
     Only matters with mods.
   - Now "\" counts as a separator on both sides, so such a path returns EACCES.
13. window_icon.bmp (launcher/assets, .gitignore, tools/export_github.sh).
   - Cause: probe_uncharted.py generates it from the game's icon0.png (Sony artwork); it had been
     tracked since 6036c2a and the export copies launcher/*.
   - Now untracked (kept on disk, ignored); the export refuses any window_icon.bmp. It stays in past commits.
14. GitHub export (tools/export_github.sh).
   - Cause: ls-files -co listed untracked files of gow3_pc, so local files such as
     stat-multipliers-test.ini went to the public repository.
   - Now ls-files -c (tracked only).
15. orb-multiplier-test and stat-multipliers-test (gow3_pc/tests/test_orb_multiplier.cpp,
    test_stat_multipliers.cpp).
   - Cause: Save() only keeps the God of War keys already in the file (it stopped writing them on
     purpose since 6036c2a); the tests expected them written from memory.
   - Now the orb test writes the keys into the file first; the stat test expects the file's own
     values. No product change.
16. Patch mismatch (tools/probe_uncharted.py, apply_patches).
   - Cause: a multi-change variant with unexpected bytes was skipped, but a single-change one raised
     SystemExit, against the "unknown game version: start without it" rule.
   - Failure case: a build whose bytes differ inside a matched pattern (U2 focus and 1 ms wait
     patterns check literal bytes inside wildcards) would not start.
   - Now it prints "Patch '...' skipped: unexpected bytes at ...", tries the next variant, then the
     usual "NOT applied" line.
17. Timed rwlocks (src/runtime_rwlock.c, acquire and rw_timedread/rw_timedwrite).
   - Cause: the guest argument was treated as a pointer to {seconds, nanoseconds}; PS4 takes
     relative microseconds (shadPS4 issue #5175). On contention the value was dereferenced as a pointer.
   - Now the argument is uint32 microseconds turned into an absolute CLOCK_REALTIME deadline; zero
     still tries once, then times out. The three games do not import these functions.
18. pthread keys (src/runtime_kernel.c, key_create/key_delete/key_set/key_get,
    runtime_thread_keys_cleanup).
   - Cause: per-thread values were kept by slot only. A deleted and re-created key reused the slot
     with other threads' old values.
   - Failure case: key_get returned another thread's stale value, and the new destructor ran on it
     at thread exit.
   - Now each slot has a generation, bumped at create; a thread's value is valid only for the
     generation it was set under. Get and set are lock-free (atomics); cleanup checks under key_lock.
19. sceKernelMprotect (src/runtime_memory.c, protect_locked).
   - Cause: the end was start + align_up(size) with start unaligned, so the last page of the
     interval was missed; huge sizes wrapped.
   - Now the end is align_up(start + size) and both additions are checked; an overflow returns INVALID.
20. God of War III script helpers (tools/graphics_options.py, most of mods.py and patches.py, their
    tests).
   - Cause: nothing in the Uncharted launch chain called them, but they shipped in the zip.
   - Now ~900 lines removed. mods.py keeps LINKED, make_link, is_link; patches.py keeps game_sfo,
     game_title_id, EBOOT_BASE, eboot_image, find_pattern. patches/*.xml left in place.

Verified: build.sh ok. runtime-test passes (all, plus new key-reuse, mprotect range and overflow,
and rwlock microsecond cases); file-mods-test passes (backslash traversal). Each new test failed with
its fix reverted (17, 18, 19, 12). orb-multiplier-test, stat-multipliers-test and
graphics-settings-test pass. Python tests game_update, prepare, content, apply_update, probe pass.
U3 1 min boot and play at 4K, MT on (build with all of 9-20): alive at end, no hang report, no patch skipped, lowest 5 s window 53.7 FPS, worst frame 149.1 ms (loading).

Not verified: a movie of another size, two threads saving at once, a genuine signed DLL in a quoted
path, a run of the export, a game build that hits a patch mismatch, the rwlock, key and mprotect
paths in game.

## 20 October 2026: audit leftovers, second audit, performance quick wins

19 commits, 978ab58..1d0593a. Evidence is from the commit messages unless marked.

### Audit leftovers (978ab58, e8b2620, 1233adb, aadc151, 3d18ab7)

- 978ab58 tools: fetch_aa_files.sh pinned the DLSS SDK to commit 9981c635 (the v310.9.16 tag's peeled
  commit; a tag can be moved) and checks 3 binaries and 23 headers against stored SHA-256 values, on
  download and for files already in downloads/. A mismatch deletes the file and fails. The .lib and
  headers are compiled into the shipped gow3_dlss.dll. Tested: bash -n; all 26 hashes match; the .lib
  and DLL match C:\dev\DLSS; fetch() passes a right hash, fails a wrong one, replaces a corrupted cached
  file. Not tested: a full script run (MSVC bridge build). The FSR4 files are pinned but not hash-checked.
- e8b2620 package: package_windows.sh read owners per DLL from pacman -Qo; an unowned DLL (other than
  gow3_dlss.dll) or an empty owner list fails the package. Before, `2>/dev/null || true` shipped such a
  DLL with no license entry. Tested: bash -n only. Not tested: a package build, the CI runner.
- 1233adb tools: export_github.sh no longer lists tests/test_patches.py and tests/test_mod_conflicts.py
  (deleted). bash -n only; the export was not run.
- aadc151 launcher: ini_lines fell back to the shared probe.ini on any OSError, so a per-game ini
  held open by another process was overwritten with the shared contents. Now the shared file is used
  only when the game ini does not exist; other read errors retry once, then read_ini returns defaults
  and write_ini skips the write (one log line each). Tested: ast parse; a PermissionError read. Not
  tested: the launcher UI.
- 3d18ab7 tools: probe_uncharted.py copies savedata to save-backups/<YYYYMMDD-HHMMSS> before each
  start, keeps the newest 5, skips when nothing changed or there is no save, never fails the launch.
  Cause: the launcher's STOP is taskkill /F and the game writes straight into savedata. Tested:
  test_save_backup (creates, keeps 5, skips unchanged, skips missing/empty). Not tested: a launch from
  the installed package.

### Second audit (7092d9e, 35fff41, f6abb5d, 1c50cf9, 79f06f2, 1b3e4b1, d57a704, 15b2bba, b7f7e47)

- 7092d9e runtime: param.bin and icon0.png go through write_file_atomic (.tmp then MoveFileExW);
  write_param, set_param, icon and mount return ERR_INTERNAL on failure; title_ok checks the title in
  mount, delete and search (path separators refused). Tested: build.sh, runtime-test, U3 1-minute
  boot. Not tested: a save/load cycle in game, a forced write failure.
- 35fff41 runtime: range_within guards direct release, discard, find_free and the low window (start +
  size could wrap past the pool). Joined threads are freed at join, detached ones reaped on the next
  create; runtime_low_map reuses freed ranges. Files are reference-counted with a per-File lock
  (close() could free a host fd under a reader; Windows pread restored the file pointer under a
  concurrent read()). Tested: runtime-test (plus --direct-bounds, --thread-reclaim), file-mods-test
  (plus --close-race, --position-race), each failing with its fix reverted (per the coder); U3
  1-minute boot. Not tested: Linux stack unmapping. A detached thread is reclaimed only when another is
  created. 47c9aeb later removed the pread serialization.
- f6abb5d gpu: TAA_DIAGNOSTICS and FixedRenderSession getenv cached; PRESET_FILE, SKIP_FILE and
  DEPTH_PROBE read by poller threads; guest-images.trigger polled at most every 250 ms. Tested:
  build.sh, graphics-settings-test, U3 1-minute boot (lowest 5 s 72.1 FPS). Not tested: each trigger
  file by hand; no frame-time A/B.
- 1c50cf9 gpu: the 16 movie scratch slots recycle least-recently-used (a 17th overwrote a slot in
  use); KeepWritable has a reference count (g_keep_count) so nested calls keep protection. Tested:
  build.sh, movie-hook-test, U3 1-minute boot. Not tested: a cutscene sequence with more than 16 movies.
- 79f06f2 gpu: SetOpen saves on a detached thread; input keys and mouse cleared on close; key-ups
  outside a capture pass through; the choice dialog and window Enter/Escape ignore key repeat;
  text_entry_active is atomic. Tested: build.sh, U3 1-minute boot. Not tested: the menu by hand with
  keyboard or pad.
- 1b3e4b1 runtime: Apply and restart was always disabled because nothing set GOW3_RESTART_COMMAND.
  On Windows runtime_restart writes the loadexec file (current executable, guest argv) and exits with
  LOADEXEC_EXIT (30); probe_uncharted.py sets GOW3_RESTART_EXECUTABLE, so its existing loop re-prepares
  (new resolution patch) and relaunches in the same process and log. Tested: build.sh; a mocked
  relaunch loop and the handoff file contents. Not tested: a live restart. Nothing restores the old
  settings if the restarted game never shows (restart_unconfirmed is not consumed; pre-existing).
- d57a704 runtime: sfo_value checks offsets in uint64_t and bounds keys with memchr/memcmp; the
  duplicate exec-watch reader (it deleted the watch file and dropped w: entries) is removed. Tested:
  build.sh, malformed SFO cases (coder harness). Not tested: exec watch in a live run.
- 15b2bba runtime: module TLS blocks (module IDs 2..7) live in the GuestThread record and free_thread
  releases them at join or reap. Tested: runtime-test, --direct-bounds, --thread-reclaim (six module
  TLS slots). Not tested: allocation counts in a live run.
- b7f7e47 tools: link_modules raises ValueError naming the file when DT_INIT is missing (it defaulted
  to the module base, i.e. the ELF header). Tested: Python tests, a missing-DT_INIT harness. The two
  errors in that run (test_packaged_vulkan, test_upscaler_assets) are pre-existing God of War 3
  leftovers: they import modules missing since the first commit.

### Performance quick wins (b1ffd84, eaa4dac, 47c9aeb, 4f39daf, 1d0593a)

- b1ffd84 runtime: GOW3_CLOCK_TRACE is read once (every guest clock read called getenv under the CRT
  environment lock); the mutex lock/unlock/wait/wakeup counters (shared non-atomic writes) are removed,
  the report keeps created counts; kernel_mprotect checks for an unchanged protection under the read
  lock and returns without the write lock, generation bump or flush_hooks. Tested: build.sh,
  runtime-test (plain, --direct-bounds, --thread-reclaim), U3 1-minute boot.
- eaa4dac gpu: the present clear and its barrier run only when the fitted blit leaves bars; PrepareFrame
  calls PumpWarmUp once per flip on the GPU command thread (before, it advanced only when the game
  looked up a pipeline; U1 entries waited ~4.8 s). Tested: build.sh, U3 1-minute boot. Not tested:
  letterboxed aspect ratios by eye.
- 47c9aeb runtime: 35fff41 serialized every pread on a file, so streaming threads read one at a time.
  Game files now open as overlapped handles; read/write/lseek keep the position in the File record and
  use positioned I/O, so pread/pwrite take no lock and never touch a host cursor; O_APPEND writes use
  the end-of-file offset; each operation waits on its own event. Tested: file-mods-test plain,
  --close-race, --position-race, new --pread-race; build.sh; U3 1-minute boot. Not tested: a level-load
  time A/B, non-Windows.
- 4f39daf runtime: stdout was unbuffered, so each printf was a WriteFile under the CRT stdout lock
  (the GPU command thread waited 548 ms for it in a level load). Now a 64 KiB buffer and a flusher
  thread every 250 ms; exit, _exit, the crash handler and the POSIX fault/watchdog handlers flush
  first (the crash handler waits up to 500 ms for the flusher, skips it if the flusher crashed).
  Tested: build.sh; U3 1-minute boot wrote the full boot.log (1810 lines). The commit message says
  the hang watchdog read the buffered log live; that was not checked (only that the log was complete
  after the boot). Not tested: an injected crash; output can be lost if a thread dies holding the
  stdout lock.
- 1d0593a gpu: U1, U2 and U3 share TITLE_ID CUSA02320, so one cache folder held all three games'
  pipelines (U3's warm-up read 2351 entries, kept 576, took 30.85 s). probe.c passes a cache id (serial
  plus executable stem from GOW3_RESTART_EXECUTABLE, e.g. CUSA02320-big3-ps4_shipping) used only for
  the shader cache and pipeline-binaries folders. Tested: build.sh; U3 boot created
  cache/CUSA02320-big3-ps4_shipping. Not tested: the U1 and U2 folders. Each game rebuilds its shaders
  once in its first session; the old shared folder is left in place.

### Performance A/B (U3)

Player settings (4K, DLAA model M, 120 FPS cap, multithreading on), C:/dev/bench/bench.sh with
SETTLE=5 SECONDS_MEASURE=10, builds alternated:

| Build | Run 1 FPS (PresentMon) | Run 2 FPS | GPU busy ms/frame |
| --- | --- | --- | --- |
| baseline b7f7e47 | 61.9 | 63.7 | 15.80, 15.50 |
| quick wins 1d0593a | 63.9 | 67.3 | 15.45, 14.68 |

Mean +2.8 FPS, -0.6 ms GPU busy. The spread between runs of the same build (1.8-3.4 FPS) is as large
as the difference: not beyond noise. Earlier 20-second baseline runs that day gave 68.6 and 63.5 FPS
(63.5 overlapped a compile).

### One freeze, not reproduced

A bench run with the baseline build b7f7e47 (which already contains 35fff41's thread reaping and
per-File refcount) stopped writing boot.log at 12:49:27, 15 s after start, during loading. The last
line was an open of u3data/build/main/effect1/sky-desert-teaser-ingame12.bin -> not found. No hang
report (the watchdog needs Frame stats lines) and no dump; the process was killed about 4 minutes
later. Four later runs (2 per build) and a 1-minute boot reached gameplay. Cause unknown. Candidates to
check first: reap_threads (pthread_join of finished detached threads in create) and the per-File
lock/refcount from 35fff41.

### Open from the performance audit (not done)

- Per-page touch faults on streaming reads (runtime_file.c touch_for_write).
- winpthreads condvars/mutexes vs SRWLOCK.
- Frame limiter Sleep granularity and present-thread spin; present thread priority.
- End-of-frame GC before work_retired.
- capture_job.Wait translating on the GPU thread.
- Motion-vector vertex shaders never cached.
- Prepare chain redone every launch (~2-3 s).
- EOP readback flushes.
- VRR/G-Sync on the 164 Hz display unverified (GOW3_DISPLAY_LOG).

## 20 October 2026: pipeline cache carried over; U3 chapter 2 frame rate

### Cache seeding (8268bac)

- Bug: 1d0593a moved each executable to its own cache folder (cache/CUSA02320-<stem>) and did not carry
  the old shared cache/CUSA02320 over. After the update the player's U3 played on an empty cache.
  boot.log showed freezes of 5.1 s (474 compiles), 4.1 s (406), 1.05 s (124), 0.94 s (90) and 0.42 s
  (26), one of them right after the museum cutscene in chapter 2.
- Fix: on first open of the new id, the old folder (or zip) is copied once, only when the new folder
  does not exist. The copy goes to "<id>.seeding" and is then renamed. It never merges, because the
  <hash>_<perm_idx>.spv names follow the discovery order of each run.
- Test: U3 bench (chapter 2 rooftops save, 4K, DLAA M, 120 cap, multithreading on) with the U3 folder
  moved aside. The new folder got all 124 MB; the run had 582 pipeline-binary hits and 13 compiles.
  Result: 64.1 FPS (PresentMon 64.0), p99 27.9 ms, GPU busy 15.07 ms/frame (96%): GPU-bound there, at
  980 draws/frame.
- Not verified: the zip mode. The "Seeded pipeline cache" LOG_INFO line did not appear in boot.log,
  probably because of the log level. The player's install already has a cold
  CUSA02320-big3-ps4_shipping (59 MB, created 20 Oct 13:16), so the seeding does not apply to U3
  there. It does apply to U1 and U2.

### U3 chapter 2, following Sully (player's log, 5 s windows)

- Earlier windows: about 4,000 draws/frame, 55-63 FPS, draw-pipe stage B 60% busy.
- The chase section: 6,300 draws/frame, 41-49 FPS, a ceiling of about 300k draws/s, stage B 79-90%
  busy, stage A waited 37%, 0 compiles. There it is CPU-bound on the draw pipe (FPS is about
  300k / 6,300). This is the known limit (see "Levels are CPU-bound on the GPU command thread"
  above).
- Correction (same day, later): that window was partly a stuck constant ring, not only the known
  limit. The ring stayed full from the alley checkpoint load until quit and pushed ~1M copies/s onto
  stage B. Fixed in 2025147 (next subsection). The CPU limit that remains after the fix is in
  "Remaining CPU limit (profile)"; it is not fixed.

### Constant ring stuck full (2025147)

- Player's 20 Oct log: 55-63 FPS at ~4,000 pipe items/frame before. A switch right after the alley
  checkpoint load (col-alley): stream copies ~300k -> ~2.1M per 5 s (~3 GB), static ranges ~1,000
  promoted and ~1,000 demoted per 5 s, known ranges 863 -> 3,259, and "constant ring ... copies/s left
  to stage B" ~1,025,000/s. Then 41-49 FPS for 4+ min until quit, at ~6,300 items/frame.
- Cause (vk_constant_ring.h, ConstantRing::Stamp): when the ring filled while no binding was reused,
  Allocate failed and the position stopped moving. Every packet then stamped the same end, and Stamp
  returned early before it published the pending end of the earlier tick. Nothing was retired again, so
  every small read-only binding (~1M/s) fell back to stage B's stream copies for the rest of the
  session. Each failed Allocate also called Retire, which refreshes the semaphore.
- Fix: Stamp publishes the pending (end, tick) pair once the tick has moved on, even with an unchanged
  end. A publish dropped because the stamp array is full (1024 entries) is retried on later packets.
  Codex and two workflow reviews (GPU safety, liveness) found no defect.
- A/B: U3 chapter 2 chase save, player settings (DLSS Performance 1080p -> 4K, model K, 120 cap,
  multithreading on). The same scripted walk to the meat market exit, then a camera turn and a 30 s
  wait. Saves /c/dev/bench/backup_ch2chase and backup_ch2meat; routes
  analysis/play/routes/chase_market.txt and meat_heavy.txt (replays drift; the scripted move
  sequence is more reliable).

| Build | Stuck sessions | Ring copies/s | Stage B busy | Stage A waiting | FPS |
| --- | --- | --- | --- | --- | --- |
| old, when stuck | 3 of 5 | 0 | 87% | 55% | 73.1 |
| old, when not stuck | 2 of 5 | not recorded | not recorded | not recorded | 75.3 |
| fixed | 0 of 3 | ~540k | 77% | 28% | 77.8, 73.6 |

- Old build when stuck: ~1.0M/s left to stage B, 1.9M stream copies per 5 s. Fixed build: brief fills
  of 5-28k/s that recovered to 0.
- Stage B cost per pipe item: ~2.8 us stuck vs ~2.3 us not stuck (+24%). That view is light (318-460
  draws/frame), so FPS there moves only ~3-5 FPS, about noise.
- Estimate for the player's heavier views (6,300 items/frame), not measured: 300k / 6,300 ~ 48 FPS
  stuck vs ~340-357k / 6,300 ~ 54-57 FPS unstuck.
- Not verified: the FPS gain in the 6,300 items/frame views. The bench's DLAA M copy was GPU-bound at
  the chase checkpoint (56.6 FPS, GPU 93%), so it cannot show a CPU-side gain there. The sample is
  small (5 and 3 sessions), so "0 of 3 stuck" is evidence, not proof.

### Remaining CPU limit (profile)

- Fixed build, market street, CPU-bound at 62-73 FPS, GPU 63-71% busy. Player's settings are DLSS
  Performance with model K (lighter GPU), which is why he is CPU-bound there. Sampling profile, 5 ms
  interval, 10 s.
- Stage B (draw recording thread) 78% busy, about 325k items/s: DispatchRecord 44% and DrawRecord
  19.5%. Inside the dispatches: BindBuffers 26.5%, ObtainBuffer 18%, GpuRangesAdd (boost::icl)
  ~10-13%, CompleteReadbacks 7%, LandGuarded 5%, NoteGpuWriteWindows 4%.
- Stage A (GPU command thread): ProcessCompute 58%, CollectRingBindings 30% (memcpy 17%),
  RefreshComputeKey -> Shader::Info::RefreshFlatBuf 22%, SyncSrtTables 22%, waiting for stage B 24%.
- The Vulkan recording thread is not the limit.
- A player stall frame had 2494 draws and 1604 dispatches. Compute dispatches dominate both stages.
- Seen in the player's stall frames, not investigated: write-fault handlers took 70-100 ms of
  80-112 ms frames at the alley load.
- Not fixed.

### Can all shaders be compiled before the game starts? (player's question)

No. PS4 shaders and their pipeline state are known only when the game draws with them. The disk cache
plus warm-up already covers everything seen once; GOW3_BACKGROUND_WARMUP=0 loads it all behind the
loading screen before play.

## 21 October 2026: freezes after cutscenes = garbage V#s specialized compute shaders

### Evidence (player's install logs, 20-21 Oct)

- U3 boot.log: 9 stalls per session, dominated by compute compiles. Worst: "Stall: 6107.4 ms frame ...
  compute variants 263 536.0 ms, compute pipelines 263 5517.5 ms". Others: 3988 ms (186), 2238 ms (99),
  1760 ms (93). U2: 2208 ms (105 compute pipelines) and 1038 ms (73). Same pattern every session.
- The player's U3 pipeline cache held 1,095 permutations of shader 8f067540 and 245 of 7d7e7206, and
  grew every run.

### Cause

- The treasure-effect compute shaders 8f067540 and 7d7e7206 read V#s from table slots that hold floats
  (the branches that use them are not taken). Binding already bound these as null (vk_rasterizer.cpp,
  "garbage V#", the 16 Oct GPU hang fix).
- StageSpecialization still keyed on their random stride/format/swizzle, so nearly every dispatch was a
  new permutation, compiled synchronously on the GPU thread (~20 ms each).

### Fix

- The binding predicate is now one function, ClassifyGarbageSharp (vk_rasterizer.cpp), also installed as
  a specialization hook (Shader::g_garbage_buffer_filter, shader_recompiler/buffer_filter.h). A garbage
  guest-buffer V# counts as null in the specialization (bitset bit clear).
- Permutations already cached still match: StageSpecialization operator== skips slots whose incoming bit
  is clear, so nothing recompiles.
- Toggle: second word bit 1024 (SpecGarbageSharps, gow3_toggles.h) restores the old behaviour, read live.
- Offline replay (Codex) of the cached .meta specializations: a mapped-base check alone leaves 53 / 13
  permutations (8f067540 / 7d7e7206); the binding predicate applied before specialization leaves 1 / 1.
- Old junk permutations stay in the player's cache and keep loading in the background warm-up (no
  stall). Clearing the cache is not needed.

### Tested / not tested

- Tested: build OK; one short U3 gameplay run from source: no crash.
- Not tested: the player's own cutscene spots (the repo saves never reach the treasure effect; the repo
  cache has 1 permutation of 8f067540), so the stall removal is inferred from the replay, not seen
  in game. No performance A/B of the extra ClampRangeSize per guest buffer per specialization. U2.

## 21 October 2026: chapter-load write faults from file reads

### Symptom

- Player report after the cutscene fix: the cutscene freezes got smaller, but loading a new chapter in
  U3 had many stalls.
- Player boot.log (21 Oct 10:19, U3, new build): 0 shader compiles in all 33 stalls. During the chapter
  load 2,000-6,348 write faults/s, "writes into GPU-written memory 0". A 103 ms frame had 96.4 ms in
  write-fault handlers and 1,049 one-page protect calls; a 215 ms frame had 203 ms in write faults; one
  5 s window ran at 39.9 FPS.
- Other stalls in the same log, not addressed here: a 230 ms frame uploading a 16.9 MB image, a 188 ms
  "Slow submit" in the driver (1,076 images registered), 97 ms frames with 58-87 MB of image uploads.

### Cause

- runtime_file.c touch_for_write stores once per 4 KiB page of each read's destination (a kernel copy
  into a protected page fails with EFAULT, so the pages must be writable first).
- Every page the GPU tracks became one write fault (exception + handler) per page.

### Fix

- Before the touches, runtime_file.c calls gow3gpu_prepare_cpu_write (gow3gpu.cpp), which calls
  Rasterizer::PrepareCpuWrite (vk_rasterizer.cpp).
- PrepareCpuWrite: a lock-free check whether any page of the range is watched. If so: interrupt hold
  (as the handler does on GPU-side threads), one drain, then for each touch address whose page is
  write-watched only, the same per-address work the write fault does. That work is HandleWriteAt =
  InvalidateMemory(addr, 8) + ExtendWriteFault + LandWatchedReadback, shared with OnWriteFault.
- Read-watched pages are left to the touches: their load takes the read fault, which lands the readback
  first.
- The touches stay as a safety net.
- Toggle: second word bit 2048 (RangeWritePrepare, gow3_toggles.h) restores the old behaviour, read
  live. Fault stats line: "file-read ranges prepared N (X MB, P pages, Y ms)".
- Rejected design (Codex review, 21 Oct): one InvalidateMemory over the whole range.
  BufferCache::ReadMemory checks only the first page, then marks the whole range CPU-modified, and the
  bulk readback cancellation rounds to pages. Both could lose GPU-written bytes. It also lacked the
  interrupt hold.

### Measured

- Repo U3 bench, Continue into gameplay (earlier version of the walk): 76 reads, 28.8 MB, 470 per-page
  calls in 34.7 ms (~74 us each). So the per-page work itself, not the exception, is most of the cost.
  The gain on the player's chapter loads is expected to be partial.
- Steady ~1,600 write faults/s while standing still in that bench come from game code, not file reads
  (0 ranges prepared then).

### Tested / not tested

- Tested: build; short U3 bench runs from source: no crash.
- Not tested: the player's chapter load, U2/U1, a toggle A/B (bit 2048 on vs off).

### Open

- Per-call cost: InvalidateMemory drains the draw pipe on each call (~74 us per page in the bench).
  Batching it safely is not done.
- Write faults from game code (~1,600/s at idle in the bench) are not addressed.
- Image-upload stalls (230 ms for a 16.9 MB image, 97 ms frames with 58-87 MB) and the 188 ms "Slow
  submit" in the driver with 1,076 images registered are not addressed.

## 21 October 2026: write faults in gameplay: prewait for GPU-written pages

Measurements (U3 bench, Continue, standing, `GOW3_FAULT_STATS=1`):
- About 1,720 guest write faults/s. `buffer_cache.InvalidateMemory` took 1,880-2,100 ms per 5 s (~235 us/call).
- Region lock wait 1.2 ms and work under the lock ~27 ms per 5 s: not the cause.
- "Fault flush" = ~746 faults/5 s into GPU-written pages: `PrewaitPageWrites` waited 1,040-1,275 ms, page
  fetch 280-370 ms, queue to the GPU thread ~260-350 ms, shared path 452 calls in 4.4 ms, downloads 0.
- Top pages 0x103f239000 (~110/s) and neighbours. Top guest RIPs 0xd25dfd2 (`mov word [rbx+0xe7dc],0`)
  and 0xd385251 (`or dword [rax+8],0x20`), guest base 0xc400000.

Cause: the 19 Oct prewait (the guest thread waits for the page's GPU work before the GPU command thread
fetches) predates the 17 Oct-era shared-page path being the common case. For writes that miss the
GPU-written bytes the shared path needs no wait, so the prewait only added one.

Fix: skip the prewait when the written range misses `gpu_modified_ranges` (`IsRangeGpuWrittenShared`,
shared lock). Toggle `PrewaitAlways` = second word 8192 restores it. Result: prewait waits 0 ms, flush
640-710 ms/5 s, buffer bucket 640-730 ms/5 s; GPU command thread blocked on GPU ticks unchanged at
27-29%; FPS 69.7-70.1 (GPU 98% busy). Codex review: approve. Note `CopyFinishedGpuPage` may now wait on
the GPU thread for submitted work in some cases (aggregate unchanged in the bench).

Tried and dropped: a bounded hot-page set (rate admission >=16 faults/s, 64-page budget, 10 s retire).
Faults -18% (8,550 -> 7,030 per 5 s) but only ~6 ms saved per 5 s and no FPS change; removed. Bit 4096
left unused.

DLSS: feature creation measured 41.1 ms (first evaluate 0.7 ms) in the bench, so the player's 230 ms
frame at chapter start is not mainly DLSS creation; cause open.

Slow submit 188 ms (`vkQueueSubmit` wall time in the NVIDIA driver): open, needs a GPUView/WPR trace.

Diagnostics added (`GOW3_FAULT_STATS=1`): Write fault split, Fault flush (phases, prewait skipped),
buffer lock wait/under lock, write-fault page and RIP histograms. Always on: "DLSS: feature created in X ms".

Open: page fetch (~300 ms/5 s) and the queue to the GPU command thread (~250 ms/5 s) per GPU-page write
fault. The ~1,500 other faults/5 s are cheap (~4 us).

## 21 October 2026: first-gameplay-frame DLSS stall; slow-submit detail

### Evidence

- Player U3 log (21 Oct, preset=3 Performance, dlss_model=k, 4K output): a 230.2 ms frame right after
  "DLSS: DLSS 1920x1080 -> 3840x2160, quality 3, preset 11, HDR input", i.e. the first gameplay frame of
  the session. Cross-checked: it is the same frame the chapter-load entry listed as a 16.9 MB image
  upload (boot.log line 349: "GPU thread on CPU 218.8 ms; 1 images registered, 16.9 MB image uploads").
  It is one frame, directly after the DLSS line above, not two stalls.
- Correction to the write-fault entry above ("not mainly DLSS creation; cause open"): that conclusion came
  from the DLAA bench (creation 37-41 ms). At the player's Performance/model K settings creation takes
  63-177 ms, so this frame was mostly DLSS creation. The older entry is left as written.
- Reproduced with the player's save and settings: "DLSS: feature created in 176.5 ms" -> 189.4 ms frame
  (1080p output run), "80.4 ms" -> 92.1 ms frame (4K run). DLAA creates in ~37-41 ms.
- NGX CreateFeature is synchronous on the recording thread; the GPU command thread only waits
  (LandDeferredFenceFor) during it. One creation per session.
- Also seen in the reproduction (repo cache only, not the player's): 88-94 ms frames with the GPU thread
  waiting in QueueShader capture_job.Wait for shaders missing from the repo cache. The player's cache has
  them (0 compiles in their stalls), so this is not their stall.

### Fix (DLSS)

- TemporalUpscaler::PrecreateDlss runs at the first frame start (boot/menus) and creates the feature there.
  It predicts the desc the first scene frame will need with the same DlssDesc builder as RecordDlss:
  SceneSize, target size, hdr = !Scaled(), QualityForScale, TransformerPreset.
- RecordDlss logs "pre-created feature used" or "did not match ..., recreating". A mismatch costs what it
  cost before.
- Toggle: second word bit 16384 (DlssPrecreate, gow3_toggles.h) restores the old behaviour.

### Verified (DLSS)

- Same save/settings, 4K: "DLSS: feature pre-created in 63.4 ms" during boot/menus (that frame 93.9 ms,
  before gameplay), "DLSS: pre-created feature used" at the chapter start, no stall frame there; 95.6 FPS,
  no crash.

### Not tested

- U1/U2.
- GOW3_RENDER_RES sessions (the likeliest mismatch of the predicted desc).
- Switching the upscaler in the menu.
- A toggle A/B (bit 16384 on vs off) beyond the before/after runs above.

### Slow submit: diagnostic only

- Player log: one 188.2 ms vkQueueSubmit in the driver during the chapter load, in the same frame as 203 ms
  of write faults, with async compile workers at 2,831 ms in that 5 s window. GPU: RTX 5070 12 GB.
- Not reproduced in 3 runs at the player's settings. No behaviour change.
- New: for submits >= 50 ms (vk_scheduler.cpp) a "Slow submit detail:" line with
  - device-local heap usage/budget (VK_EXT_memory_budget; "heap budget unavailable" without it),
  - compiles running right now (new g_gow3_compiles_running counter in the compiler workers,
    vk_pipeline_cache.cpp),
  - the 5 s window compile totals (async workers, GPU thread compiles), read without resetting them,
  - submits not yet known complete and pending ops.
- Hypotheses it separates: VRAM over budget (the driver pages inside the submit) vs. pipeline compiles
  contending in the driver.

### Open

- Slow submit stays open until a player log shows the "Slow submit detail:" line.
- The chapter-load entry's 230 ms frame is the DLSS one above (mostly DLSS creation, not the
  16.9 MB image upload). The other stalls from that entry, 97 ms frames with 58-87 MB of image uploads, are not addressed here.

## 21 October 2026 (afternoon): post-movie stall, alley-load stalls, chapter 2 FPS

### Evidence

- Player report: the chapter-start stall is almost gone; the stall after the cutscene is smaller but still
  there; then a big FPS drop.
- Player log 21 Oct 12:51, 297 s of frame times. U3 chapter 2, DLSS Performance 1080p -> 4K, model K, 120
  cap, MT on, RTX 5070, window 2560x1440.
- P1, post-movie stall: at 203 s, right after a ~60 s pre-rendered movie, a 99.7 ms frame with "image
  refresh 63.1 ms" (17 images, 21.3 MB), then five frames of 42-55 ms.
  - During the movie the staging pool's large buffers went from 14 (69 MB) to 0 (trim after 3000 idle
    frames, ~25 s); 20 were created again after it.
  - Chapter-start frames by contrast uploaded 105 MB with 4.7 ms of refresh (buffers still cached).
- P2, alley-load stalls (278-280 s): 100.7 and 80.3 ms frames with 89.4 / 74.0 ms in write faults. Window
  stats: "file-read ranges prepared 239 (92.1 MB, 5323 pages, 468.9 ms)" (~88 us/page), 5,681 page fetches
  on the GPU command thread (485.7 ms) plus 398.2 ms queued to it.
  - Cause: a file read replacing a whole page first fetched the page's old GPU bytes, one GPU-thread
    command per page.
- P3, FPS drop: after the movie 52-80 FPS at 1,600-3,000 draws plus 1,600-1,900 dispatches per frame. GPU
  command thread idle 0.2%, stage B 83% busy, stage A waits 20% for it, ~285k pipe items/s. This is the
  known CPU limit ("Remaining CPU limit (profile)", 20 Oct).

### Fixes

- 27da750, diagnostics (no behaviour change):
  - The stall line splits image refresh into staging allocations, inline image copies, detile (pipelines).
  - New lines: "Slow staging allocation" (>= 5 ms) and "Detiler pipeline ... created in X ms" (bench:
    0.1-6.0 ms each, not the cause).
  - The old "staging" / "waiting for host copies" columns were never incremented (always 0.0).
  - Windows "us/draw" in Frame stats is now real (GetThreadTimes on the GPU command thread).
- 06fdcac, P1: TrimLarge keeps the most recently used idle large staging buffers up to
  GOW3_STAGING_KEEP_LARGE_MB (default 128) and 32 per memory type; 0 = old trim. Memory cost: up to 128
  MiB per memory type kept.
- 999e363, P2: a file read discards whole pages in one GPU-thread command per read instead of fetching
  each page's old GPU bytes.
  - Discarded: gpu ranges, page tick, page_writes, lazy/shared records; region marked CPU-modified;
    texture invalidate.
  - Refused pages keep the old path: pending readback piece, overlapping GPU-modified or
    download-queued image (new TextureCache::PageHasGpuImageAuthority), edge pages, read-watched pages.
  - runtime_file.c clips the prepared range at EOF.
  - Disabled2 bit 32768 (FileReadDiscard) restores the old path. The fault stats line counts discarded
    pages.
  - Codex static review: buffer paths, drain and lock order OK. It found the image case (now refused)
    and the short-read limit. Documented shortcut: a read that fails after the discard leaves those
    pages without their GPU bytes.

### Measured

- Bench, creating one large staging buffer (new diagnostics): 38.9 and 33.2 ms (64 MiB DeviceLocal), 30.2
  ms (32 MiB HostUncached), 15.4 ms (24 MiB DeviceLocal). A 16 MiB DeviceLocal ring block 9-13 ms. The
  movie stall itself was not reproduced in the bench.
- Bench, chapter 2 meat market save, Continue, file reads: "57 (20.4 MB, 427 pages, 339 discarded, 2.2
  ms)" vs "67 (23.7 MB, 532 pages, 50.1 ms)" before.
- Bench standing at the meat market: 67-75 FPS at ~700 draws/frame with 19 us/draw of GPU-thread CPU
  time (1,862 dispatches/frame). Walking into the next area: 49-57 FPS at 2,000-2,750 draws/frame.
- Sampling profile, current build, meat market, 10 s windows, 5 ms:
  - Stage B: RunDrawPacket 73-77%, DispatchRecord 44-49%, BindBuffers 27-29%, ObtainBuffer 20.4-20.8%,
    GpuRangesAdd 9.9-10.1% (boost::icl 8.5%), CompleteReadbacks 8.7-8.8%, LandGuarded 5-6%,
    NoteGpuWriteWindows 3.5-3.9%.
  - Stage A: ProcessCompute 54-60%, CollectRingBindings 33% (memcpy of guest constants via
    CopySparseMemory 18-19%), DrainDrawPipe (waiting for B) 15-19.5%, GetComputePipeline 12-13.6%
    (RefreshFlatBuf 8.7-10.9%), SyncSrtTables 8-10.5%.

### Tried and dropped

- Not committed: a memo that skipped GpuRangesAdd and the per-readback rewritten adds for a range already
  recorded in the same scheduler tick.
  - Hit rate ~0.1% (about 2,400 hits vs 2.0 million misses per 5 s): each dispatch writes different
    ranges.
  - A/B 3+3 windows of 10 s: on 75.68 vs off 76.79 FPS, diff -1.10 [-2.15, +0.01]. Removed.
- The FPS limit remains. A real gain needs structural work (batching the GPU-range bookkeeping per
  submission, constant-ring reuse across submissions, splitting stage B). Not done.

### Slow submit

- Reproduced in the bench: "Slow submit: 266.1 ms in the driver (0 wait semaphores, 1 signal); 295 images
  registered since the last slow one".
- Detail line: heap 0 3423/11175 MB (31%), compiles running 0, no compile work in the window. That frame
  took 283 ms; GPU command thread kernel time 109.4 ms.
- So neither VRAM over budget nor compile contention. Still open: kernel time inside vkQueueSubmit after
  many new images; needs a GPUView/WPR trace.

### Not tested

- U1 and U2.
- The player's own movie moment (no save before it): P1 fix not seen on the real case.
- The player's own alley load.
- A toggle A/B for bit 32768 (FileReadDiscard).
- One bench run ended during the chapter load without a crash message (play.py saw no process, then
  killed it); cause unknown. The next two runs passed.

### Open

- P3: the CPU limit of stage B / stage A in heavy U3 areas (52-80 FPS at 1,600-3,000 draws). Needs the
  structural work listed above.
- Slow submit (266.1 ms in the bench): cause open, GPUView/WPR trace needed.
- P1: memory use with up to 128 MiB per type kept, over a long session, is not measured.
- P2: short-read limit (a read failing after the discard leaves those pages without their GPU bytes) is a
  documented shortcut, not removed.
- The unexplained bench run that ended during the chapter load.

## 21 October 2026 (evening): chapter 2 CPU limit, SRT pipe drains

### Evidence

- Profile (meat market, GOW3_PROFILE=10, 5 ms): once stage A's ring memcpy was off it, stage A waited
  37% on stage B, 22.8% at the SRT pipe check (SyncSrtTables operator()), ~1,400 rasterizer drains/s;
  only 1 real SRT table download per 5 s.
- The 256-range pipe_write_ranges list overflowed after 256 written buffers (~1,800 dispatches/frame),
  and every later SRT walk drained the pipe: 4,715 of 4,722 drains per 5 s were overflow drains
  (GOW3_FRAME_STATS "SRT pipe check").

### Fix

- Uncommitted 20 Oct work from another session (main checkout) ported and committed: SyncSrtTables
  checks pending GPU writes (PendingWriteOverlaps), pending labels (NotePendingLabel) and un-noted
  queued tasks instead; the old check stays while pending_complete is false.
- Disabled2 bits SrtPipeCheck 128, DrainCopyShaders 256, SrtPendingWrites 512 (replace first-word
  1<<61 / 1<<62, which collided with OneFrameStart / FullScreenPassJitter).

### Measured

- U3 ch2 chase save = Cartagena market, Continue, standing; player's ini preset=3 dlss_model=k, 4K, 120
  cap, MT on; ab2walk.sh 3x10 s windows each state.
- SrtPendingWrites on vs off: 65.01 vs 61.99 FPS, +3.03 [+1.17, +4.61]; median -0.78 ms [-1.16, -0.34];
  p99 -0.65 ms [-1.10, -0.01]. With it on: 0 SRT drains.
- Market landing: 1,710 draws/frame, ~60 FPS, GPU 63% busy, stage B 81% busy: CPU-bound.

### Tried and dropped (not committed; patch kept outside the repo)

- RewrittenClip (pending readbacks' rewritten sets get only the bytes inside their pieces) +
  GpuRangesContained (skip icl add of an already GPU-modified range): meat market +0.25 [-0.78, +1.40];
  market on top of the SRT fix +1.62 [-0.14, +3.82].
- RingCopyThreads (stage A's guest constant-ring copies on the copy threads, one shared batch every
  host-copy wait starts): meat market +0.64 [-0.24, +1.59]; market on top of the SRT fix +0.58
  [-2.70, +5.62]. Reviews found three ordering defects (batch handed to the pool after unlocking;
  stage-A guest writes without a host-copy wait; GpuIdle/flip signals without one), fixed before
  measuring. Remaining gap: compute ring read_addr updates.
- Neither stage alone is the limit: cutting stage A's memcpy moved its time into SRT drains; cutting
  stage B's bookkeeping moved little. Stage B now 81-87% busy.

### Not tested

- U1, U2; the player's own run; long soak with the SRT change.

## 21 October 2026 (evening): upstream fixes ported (batch 1)

Four commits taken from BilalReloaded's repo one by one (no merge).

- 2959f6d gpu: impossible images skipped; border table at 0. Source: upstream 0.4.1 (512676a).
  Files: vk_instance.h, vk_rasterizer.cpp, texture_cache/sampler.cpp. Test: build.sh exit 0; U1 and
  U3 40 s boots, no exception. Not tested: the hour-long U1 session (#63), U2's crash scene (#56).
- 220268e games: constant tables touched before the fill. Source: upstream 0.5.0 (3b23bf9). Files:
  tools/probe_uncharted.py (CUE_PRETOUCH_PATCHES). Test: U1 and U3 (1.02) 40 s boots, 2 changes
  each, no exception. Not tested: U2; the crash itself (needs the reporters' Windows 10 stack layout).
- 63e0a9b games: idle job workers sleep on the other retail 1.02 build. Source: upstream 64abc3f.
  Files: tools/probe_uncharted.py (JOB_SLEEP_PATCHES). Test: U1 and U3 boots on our dump, same
  patch offsets. Not tested: the EP9000 build; our extra 1 ms wait patch still matches only the r15
  form.
- f8d8aa2 u3: screen wipe covers the whole screen below 720p. Source: upstream 696bcb1. Files:
  tools/probe_uncharted.py. Test: the script parses. Not tested: a sub-720p run.

### Not taken yet from upstream

- Memory trimming, pageable fallback, nested command-list leak.
- The multithreaded flicker fix (draws wait for the second-thread constant copy, #23).
- Shader cache checksum and per-GPU cache.
- Player features: 5.1/7.1, trophies, unlock rewards, light bar, .pkg install.
- Character motion vectors (needs reconciling with our flat camera source).

A full merge of upstream gives 19 conflicting files. Upstream has its own v0.5.0 tag on 3b23bf9
(ours is dcae6ae).

## Upstream (BilalReloaded) 0.4.1 / 0.5.0 notes, merged 21 Oct

BilalReloaded's STATUS sections after the merge base 6318dfb, as they wrote them (headings one
level down). Their dates are theirs; fork merge choices are in the fork notes above.

### 20 October 2026: U1 0x64011f crash (red zone) and the nested-task leak

- **"execute access to 0" called from 0x64011f** (U1 1.02; #27, #30, #42, #62): `call rcx` at
  0x64011d walks a constant-update table (0x28-byte header, 0x30-byte entries, the 16-bit word at
  entry +0 selects a member function pointer). The table is filled by a leaf function (U1 0x63df10,
  U2 0xd645f0, U3 0x12d6cf0; same code in 1.00) that keeps running entry indices and pointers in
  the red zone ([rsp-4] .. [rsp-0x2c]). A table that crosses into a GPU-tracked, write-protected
  page faults inside it; the Windows x64 exception frame may be built right below rsp, a counter
  comes back wrong, an entry keeps a stale word and the walk calls null. Measured on this PC
  (Windows 10.0.26300, scratchpad cue/deep.c): the dispatch writes nothing above rsp-1648, which is
  why the bug never showed here. Fix (probe_uncharted.py CUE_PRETOUCH_PATCHES,
  UNCHARTED_CUE_PRETOUCH=0 turns it off): the function's first 5 bytes jump to a stub in a dead
  assert path nearby (still int 0x41 first) that does `lock or byte [p], 0` on every page of
  [rdi, rdi + 0x28 + 0x30*edx), then the moved pushes and back. Stub tested standalone
  (scratchpad cue/stubtest.c: a leaf with a red-zone value, a handler that zeroes rsp-48..rsp: the
  value is lost without the stub, kept with it, faults move into the stub); U1/U2/U3 run at 60 FPS
  with it, no faults.
- **Host memory leak** (liverpool.cpp): nested IndirectBuffer / IndirectBufferConst tasks stop at
  final_suspend and were never destroyed (only top-level tasks were). `task.handle.destroy()` after
  the three wait loops. U3 bench: private bytes +344 MiB in 135 s before, +26 MiB after (flat in the
  last minute), 60 FPS both.
- **Device-loss report from Semaphore::Refresh** (vk_semaphore.cpp; GitHub #67: the loss was first
  seen reading the master semaphore counter): ErrorDeviceLost there now dumps the GPU checkpoints
  and the device fault info before the assertion, as the waits and the submit already did.
- **Effect shader variant storms** (GitHub #71/#79; shader_recompiler/resource.h
  IsGarbageEffectBuffer, Shader::gpu_mapped_extent set in the Rasterizer constructor;
  GOW3_EFFECT_SHARP_FILTER=0 = old): cs 8f067540 (effects + U3 crowd skinning) and 7d7e7206 read
  V#s from table slots that hold floats for some effects. BindBuffers bound them as null, but
  StageSpecialization and the translation still saw their random stride/format: every new float
  set made a variant plus a compute pipeline on the GPU thread. Our cache: 8f067540 1,177
  variants (all 9 slots with 450-590 distinct stride/format values), 7d7e7206 364; the user's
  18 Oct U2 session had a 7.8 s frame with 1,112 compiles right after the garbage V#s started.
  BufferResource::GetSharp now returns Null for these two shaders when BindBuffers' garbage rule
  holds (unaligned, stride >= 8 KiB, > 64 MiB, or not fully mapped with num_records < 0xFFFF0000),
  so specialization, translation and binding agree. Checked: all 95 garbage V#s logged in the
  three sessions that made the variants are caught; the stride rule alone cuts the cached keys
  1,177 -> 399 (364 -> 84); U2/U3 from empty caches and warm, 60 FPS, no faults. The U2
  Treasures screen did not produce garbage V#s on 20 Oct, so no live A/B.
- **Pageable video memory** (GitHub #81, MX350 2 GB: `ErrorOutOfDeviceMemory` from the buffer
  arenas' vkAllocateMemory). The sparse arena buffer accepts only device-local memory on NVIDIA
  (RTX 5080: type 1 only; temporary "GPU: arena memory type N allowed" lines in
  BufferCache's constructor). vramfill test (analysis/test-tools/vram): NVIDIA refuses device-local
  memory at 15,360 MiB without VK_EXT_pageable_device_local_memory and hands out 24,576 MiB (the
  test's maximum) with it; the limit is per process (a second process holding 12 GB changes
  nothing); held device memory also charges system commit (13 GB held: +13.4 GB commit).
  vk_instance.cpp now enables VK_EXT_memory_priority + VK_EXT_pageable_device_local_memory when
  supported ("GPU: pageable video memory on"; GOW3_PAGEABLE_MEMORY=0 = off). U1/U2/U3 normal runs
  60 FPS, no faults. Test knob GOW3_TEST_VRAM_HOG_MB=N holds N MiB at device creation: with it
  (11,000 or 13,300, pageable on or off) U1 dies at start with STATUS_STACK_OVERFLOW (exit
  0xC00000FD) about 2 s in, before the first frame, with no access violation (GOW3_FAULT_TRACE=1)
  and no log line: cause unknown, test-only so far (a real 2 GB card boots). Open lead.

### 20 October 2026 (afternoon): PR #87 merged (job sleep on game version 1.00)

kuraeow's #87 (merged on GitHub as 5cc4ea0, applied to the local tree; backup of the previous
tools/probe_uncharted.py in the session scratchpad) adds a second form to two of the three
JOB_SLEEP_PATCHES sites. The PR calls it "another retail 1.02 build"; in our dumps the new forms
are exactly the 1.00 executables (game/CUSA02344, all three games: one match each; 1.02: none):
- re-check before waiting: `mov rax,[r13]` instead of `[r15]`; the 1.00 timed-wait wrapper
  (U1 1.00 0x362420) multiplies its argument by 1000, so `mov edx,10` is the same 10 ms as 1.02's
  `mov edx,10000` passed straight to the wait.
- submission: `cmp byte [rdi+0x59],0; je short` instead of `[rax+0x59]` + `je rel32`.
Before: 1.00 players got only the worker-loop part (workers slept, woken only by the 10 ms
timeout). Offline check (apply_patches on copies of the six executables, old vs new script):
1.00 all three parts patched in U1/U2/U3; 1.02 output byte for byte the same as before.
No game run (the user was playing). CHANGELOG "Next version" entry added.

### 20 October 2026 (afternoon): 10 player crash logs (downloaded with the user's OK)

analysis/test-tools/issues/i<issue>/boot.log; summary script analysis/test-tools/issue_summary.py.
- **4 of 10 = U1 0x64011f** ("execute access to 0 at 0", host #1 guest offset 0x64011f, same
  guest stack 0x641532 <- 0x501ea9 <- 0x56d027 ...): #78 (RTX 3050 Laptop 4 GB), #82 (RX 9060 XT),
  #85, #90 (RTX 5060 Laptop 8 GB). That is the CUE red-zone crash fixed by
  CUE_PRETOUCH_PATCHES (unreleased; 0.4.1 does not have it). The "crash after 10-30 min on 4-8 GB
  laptops" pattern is this crash, not video memory: no VRAM pressure in #90, #78 steady ~2.9 GB.
- U1 0x5f64f3 read of 0 (#59), U2 0xd09665 read of 0 (#61), U1 0x63f61e read of -1 (#34),
  RX 9070 boot "ASSERTION: root" + read of -1 at 0x4dd8ca (#80): open.
- Device lost: #57 (0.3.1, garbage 6755-layer image, fixed in 0.4.1 by PlausibleTarget), #76
  (RTX 5080 4K, U3 chapter 8): open.
- Performance: #61 (U2, RTX 4070 SUPER, 2880x1620) guest-thread read faults on readback pages
  445 per 5 s costing 426 ms (8.5% of the time), "23590 recurring readback pieces"; FPS min 25.7,
  worst frame 6.5 s. #34 (U1, older build) 1,850 compiles, worst frame 2.5 s.
- Arena residency is never released (EnsureResident only adds; nothing removes resident_ranges,
  also not on unmap): real, but not what these logs crash on. Keep as a later 4 GB-card item.

### 20 October 2026 (afternoon): performance work in progress (2 workers), notes

- Worker A (done, built, not deployed): trigger files polled at most every 250 ms (TriggerPollDue in
  vk_rasterizer.cpp; vk_frame_capture.cpp; vk_temporal_upscaler.cpp), stream-copy stats clock once
  per submission, write-fault statistics only with GOW3_FAULT_STATS, small-GPU-write stats
  lock-free once printed, IsComputeMetaClear cheap tests first, CopyBuffer watch lookups only with a
  watch range, PublishGraphics early return + failed_graphics map, fetch shader data assigned in
  place (vk_pipeline_cache GetProgram out-parameter, FetchReset), getenv cached in runtime_kernel.c
  (every guest clock read), runtime_audio.c, runtime_sema.c. Reviewed by me against
  analysis/backups/src-before-perf-20oct: behaviour-identical. Also mine: shim/common/debug.h
  TRACE_* no longer evaluate their argument (texture_cache formatted a string per image refresh).
- Not done (risk found on the second check): dropping the dead 8f067540/7d7e7206 variants from
  players' caches by a per-shader meta version. New variants would reuse the old perm_idx names
  (_0.spv, compute key HashCombine(hash, perm)), so a stale pipeline binary could pair with a new
  module unless pipeline binaries are keyed by the SPIR-V. Needs that check first.
- PR #84 (open), new concern, checked twice: LoadShaderMeta never loads motion vertex shaders
  (session-local buffer device addresses), and #84 sets sel.motion for every Uncharted G-buffer
  draw (stencil + depth write, one target) -> every G-buffer vertex shader variant is compiled
  again in EVERY session (not only the first). Decide "character draw" before pipeline selection,
  or make the motion VS address-independent (push constant / descriptor) so it can be cached.

### 20 October 2026 (evening): US copy (CUSA02320) tested, E:\uncharted usa

The user's US PKGs (base 43 GB + update 1.02, 1 GB): base unpacked with tools/pkgtool PkgTool
pkg_extract to E:\uncharted usa\CUSA02320 (3 min, 59 GB, same uroot layout as CUSA02344); the
update with the launcher's installer (tools/install_update.py) as CUSA02320-patch (separate
folder, the layout of #43/#29/#58/#73).
- PkgTool pkg_extract leaves out the PKG's metadata entries: the base had no sce_sys/param.sfo,
  playgo-chunk.dat/.sha, playgo-manifest.xml, shareparam.json, so install_update.py refused the
  folder ("Choose the game folder first"). Added them with install_update's own entries() +
  pkg_extractentry (METADATA list). A player who unpacks the base PKG himself hits the same.
- With the current build (deployed before-perf exe): U1, U2, U3 all start (title, 60 FPS) through
  the links view (E:\uncharted usa\.uncharted-views, eboot = the update's, sha1 d81e699f...) and
  with UNCHARTED_TEST_OVERLAY=1 (runtime overlay, 171 files). Without the base's metadata the game
  still starts (playgo-chunk.dat comes from the update's sce_sys). So the US start crash does not
  reproduce here; cause still unknown (#82 is US + 0.4.1 and got in-game).
- Idea for players with PKGs: launcher "Install game from .pkg" (base: pkg_extract + metadata).
- 20 Oct evening: launcher Advanced -> FOLDERS -> "Install game from .pkg" (launcher
  install_game_pkg -> tools/install_update.py install_game; CLI `--game <pkg> <folder>`): pkg_extract
  into <folder>/<TITLE_ID>, metadata entries added to uroot/sce_sys, base files checked, update PKG
  refused (param.sfo CATEGORY gp; base is gd), existing install refused. Tested with the US PKG
  (2 min 49 s, then removed). US 1.02 executables are byte-identical to EU 1.02 (all patches apply).
- 20 Oct evening, US gameplay check (new build out/gpu, GOW3_TEST_CHOICE=0 answers the port's save
  list dialog, new test knob in shim/gow3gpu.cpp): New Game in all three games from the US copy:
  U1 chapter 1 boat (60 FPS, worst 17.2 ms), U2 chapter 1 train cliff (60 FPS, 4+ min), U3
  chapter 1 bar fight (60 FPS; one 65 ms frame at the level load). No faults.

### 20 October 2026 (evening): hybrid-laptop freezes = present under the submit lock (fix built)

- Laptop (7840HS, NVIDIA laptop GPU + 780M; the 780M drives the 2560x1600
  165 Hz panel, frames copied across): 17:30 build, 1080p windowed: U3 median 38 FPS (min 24),
  freezes to 0.82 s; U1 Continue median 47 (min 32), freezes to 0.61 s; menus 46-184 ms freezes.
  In the freezes the GPU command thread was not idle, in no counted wait, ~31 ms on the CPU.
- Cause in code: Presenter::Present / PresentLoadingFrame called vkQueuePresentKHR holding
  Scheduler::submit_mutex (present queue = graphics queue), so every SubmitExecution waited for a
  blocking cross-adapter present. Fix (built 19:21, out/gpu, sha1 1e31b0d7): a third queue of the
  graphics family for presents (vk_instance.cpp; Instance::SeparatePresentQueue; PresentLock in
  vk_presenter.cpp), GOW3_PRESENT_QUEUE=0 = old. New counters Gow3Stats::frame_wait_ns /
  submit_lock_ns (draw scheduler only) / acquire_ns / present_ns: "Presentation waits:" line per
  5 s and a "presentation:" line per Stall block (driver.cpp). AMD (1 graphics queue) unchanged.
- Desktop check (5080, screen on the card): U3 and U1 gameplay 60 FPS, worst 17.2 ms (U1 one
  38 ms), "presents on their own queue", present 1% of wall time, saves restored. Usual build
  (before-perf) deployed again. Backups: analysis/backups/main/present-queue/.
- Package for the laptops: analysis/laptop-pkg/fix-20oct-2(.zip, 34.6 MB): exe, present-queue.patch,
  LAPTOP-HANDOFF.md (pq-on/pq-off A/B twice in U3, U1, U2, stall stacks, 780M). Laptop share access
  failed both ways (laptop port 445 closed; this PC refuses the laptop: account restrictions).
- 5060 laptop: no results today; its session was offline at 19:40.
- Still open: U3 ~48 FPS ceiling on laptop CPUs (GPU command thread CPU-bound, ~54K draws/s).

### 20 October 2026 (night): laptop freezes = port on a USB exFAT drive

- Laptop: D: = NVMe SSD in a USB enclosure (ASMT 2462, exFAT); C: = internal NVMe.
  The port tree (D:\uncharted) and first the game (D:\game, /app0 overlay) were on D:.
- Present-queue theory disproven on the laptop (Presentation waits ~0% with and without it).
- Game on C:, port on D:: U3 steady 53-55 FPS but freezes to 0.67 s after the level load;
  U1 worse (28-60 FPS, 0.5 s freezes every window). Port AND game on C: (port copied to
  C:\Users\bilal\Desktop\uncharted\port, 3.2 GB): U3 1 frame over 40 ms (43.9), 0 over 100 ms.
- Freeze stacks (port on D:): GPU thread 61% waiting for the guest, ~13% in trigger-file checks
  (filesystem::exists/remove on D:, ~100 ms each); game flip thread waiting on a guest condvar.
- Launcher: slow_drive_reason / confirm_slow_drive (USB/SD/MMC/FireWire bus via
  IOCTL_STORAGE_QUERY_PROPERTY, removable, FAT/exFAT): warns once per port+game folder pair.
  Checked here: D: (WD My Passport USB HDD, exFAT) flagged, NVMe drives not. CHANGELOG entry.
- Open: make the port immune (async log writes, no trigger polls on slow drives): pinpoint run
  dport-guest-u3 requested; CPU-bound U3 ~47-54 FPS (NVIDIA GPU) / 41 (780M 720p): optimization worker.

### 20 October 2026 (night): GitHub #95 memory, proper fix (built, laptop test pending)

- #95 log (0.4.1): compute pipelines 926 -> 9,942 over the session (variant storm of cs 7d7e7206 /
  8f067540, fixed in the tree: laptop soak 40 -> 47). RAM flat in steady state on the laptop
  (8.46 GB private for the last 6 of 16 min) = the host leak fix holds.
- Remaining: buffer arenas never release (soak 320 -> 1,920 MiB in 16 min, one level change).
  Fix (fix-20oct-6, sha1 06725fb6, 21:40): BufferCache::ReclaimArenaMemory (per submission from
  Rasterizer::OnSubmit, works every 2 s): 1 MiB windows of guest GPU memory get the epoch of their
  last arena use (TouchArenaWindows in GetArena and SynchronizeDmaRange). Above GOW3_ARENA_LIMIT_MB
  (default max(768, heap/4)), windows unused for GOW3_ARENA_RECLAIM_AGE_S (60) are released oldest
  first down to 3/4 of the limit: skipped when GPU-written (gpu_modified_ranges, tracker), pending
  readbacks, lazy pages, or the GPU has not finished the epoch after their last use.
  ReleaseArenaBlocks: IntervalList::Remove (new, with a cut functor: Backing::SubRange mixes block
  and byte units), sparse unbinds into pending_binds, BDA page table entries zeroed, guest range
  marked CPU-modified (re-upload; static ranges demote via IsRegionCpuModified), memory runs
  quarantined until SubmitPendingArenaBinds, then reused first by EnsureResident (which also trims
  pending unbinds of ranges bound again). GOW3_ARENA_RECLAIM=0 off. Memory: line shows held / in
  use / given back. Risk: shaders in DMA "full" mode reading a released window see zeros (as for
  memory never made resident); mitigated by the 60 s age and the limit.
- Backups: analysis/backups/main/arena-reclaim/. Laptop runs: forced-u2/u3/u1 (limit 600, age 20,
  screenshots), default-u2 900 s, 780M u2.

### 20 October 2026 (night): CPU savings confirmed on the NVIDIA GPU; 780M memory placement

- fix-20oct-5 A/B on the NVIDIA GPU (U3, C: port), two pairs: GOW3_READBACK_POOL + GOW3_SKIP_COMPUTE_REBIND
  on vs off: ~1,030 draws 54.5 vs 50.9 FPS (+7%); ~1,250 draws 65.3K vs 62.0K draws/s (+5%).
- 780M (pq-on-1 log): sparse arenas allowed in type 0 (heap 0, 256 MiB, DeviceLocal), 1 (heap 1,
  19.8 GB system, HostVisible|Coherent), 2 (heap 2, 256 MiB, DeviceLocal|HostVisible), 3 (heap 1,
  +HostCached). The arenas went to the 256 MiB heap (overcommitted by 1-2 GB).
  fix-20oct-7 (22:00, sha1 c7ebe32f): GOW3_IGPU_ARENAS=system (integrated GPUs, opt-in) picks the
  largest HostVisible|Coherent heap (prefers HostCached), arena_host_mapped; reclaim limit there =
  max(768, min(2048, heap/8)). GOW3_WAIT_PAGE_READ (on): with host-mapped arenas and no copy queue,
  a CPU write fault waits for FetchableWindowTick then ReadFinishedGpuPage instead of a full
  download. Also: SrtPlanFor memo (32 entries), StaticRangeBits 18 -> 16.
- 780M U3 720p, fix-20oct-7 (same view, ~1,028 draws): default 43.9/45.1 FPS; GOW3_IGPU_ARENAS=system
  47.2 (and 44.4 at 1,195 draws = 53.5K draws/s), GPU ticks 3-3.7% -> 0.1-0.5%, 181 direct page
  reads/5 s, no faults -> made the default for integrated GPUs (22:21 build; =local: old).
  System + multithreaded (UNCHARTED_MULTITHREADED_U3=1 UNCHARTED_U3_MULTITHREADED_TEST=1): 55.1 FPS
  (min 53.9, worst 29 ms), stage B busy 62%, stage A waited 18%, no faults. 15-min MT soaks on
  both GPUs requested before changing the U3 multithreading default (#51 freeze risk).
- 780M U2 subtitle / pause menu flicker (user, watching): bis-1 system arenas yes, bis-2 local
  arenas yes, bis-3 READBACK_POOL/SKIP_COMPUTE_REBIND off yes, bis-4 laptop's pre-20-Oct exe
  (cc6f758b) yes = pre-existing, not today's work. bis-5 (NVIDIA GPU at 720p) / bis-6 (780M at 1080p)
  split GPU vs render size. System arenas made the iGPU default again (23:04 build).
- Desktop (user, 5080, usual build f0ec3cbf, U2, DLSS 4K render): aim blur back. Log: "Aim focus:
  crosshair box centre 0.25 -> 0.5 (render 3840x2160)" applied, "Tiny image readbacks: ~300/5 s"
  (last 0x1040340000). Read-only live check: scratchpad u2focus.py (global 0x13e42c0+0x35f8).
- U2 aim blur (user, 5080, 4K DLAA): live read (u2focus.py) of the focus value: alternates between
  2.06-2.18 (target, tracks aiming) and 5.637/5.641 (= the uncorrected box: the wall beside Nate,
  16 Oct analysis). FixAimFocusCentre wrote the corrected centre through the backing view only
  (not tracked): when another draw had uploaded the block earlier in the frame the GPU kept the
  old centre. Now BufferCache::WriteThrough (guest + GPU copy in stream order), backing fallback.
  Build 23:10 deployed to out/gow3-probe.exe for the user's U2 check (U3 uses the same path).
- 780M flicker: bis-5 (NVIDIA GPU at 720p) no flicker -> 780M-specific, pre-existing.
- U2 aim blur FIXED (user-confirmed 23:24, 4K DLAA): FixAimFocusCentre now writes through
  (BufferCache::WriteThrough). New per-5 s line "Aim focus (5 s): N binds; skipped ...; N corrected
  (N through to the GPU copy)": user session 300/300/300. Build 6a3bb583 (23:20) deployed.
- U3 d-pad prompt (user screenshot, museum chapter): picture 256x256 BC1 hash e8a2f49b992621fc
  (masks f63e4fb97d71d9ed / 31e0b53a6c874ec5), found with GOW3_PROMPTS_LOG=1 (test-env.txt, restored)
  and scratchpad promptsheet.py (reads the logged pictures from the running game, read-only).
  gow3_prompts.cpp Kind::Dpad + RedrawDpad: the bound key (GOW3_PROMPT_UP) on the lit orange arm,
  keyboard set only (controllers keep the picture). prompts-test harness takes "dpad"; rendered OK.
  Build 00718657 (23:39) deployed; user check pending.
  User-confirmed fixed (23:50).
- Laptop mt-soak-nv (fix-7, U3 MT, 15 min): FPS 60 most of the time (MT: GPU thread idle 23-27%)
  but dips: compute variant storms (cs 8f067540 / 7d7e7206 / 0c936dd6 each 100+ variants, compute
  pipelines 40 -> 733, a 918 ms frame with 136 compiles). 0c936dd6 had unmapped 1-3 GB V#s (bound
  null, but specialised). Fix (fix-20oct-8, 23:53, sha1 34d1b37b): IsGarbageEffectBuffer for EVERY
  shader (odd-looking V#s that BindBuffers would null read as null in GetSharp); effect family
  (now incl. 0c936dd6, IsEffectShader) keeps the strict rule; "variant N buffers" diagnostics at
  101-106 variants. f8 MT soaks (NVIDIA GPU, 780M) queued. Deployed on the desktop too.
- U3 museum red carpet (user): pre-existing (the pre-20-Oct build shows it too). Frame capture
  analysis/uncharted-user/captures/frame_20261020-235100_2.txt (scene pass #22: 98 draws, 125
  textures); cause not found yet.

### 21 October 2026 (night): U3 museum red floor = damaged shader cache pair (FIXED, verified)

- Symptom: the room behind the guard's door (young Drake, museum) shows a flat red floor on the
  desktop (RTX 5080) at any resolution/upscaler/draw distance; the laptop renders its green/yellow
  tile pattern. Not vendor, not depth: GOW3_PIPELINE_CACHE=0 (fresh compile) renders it right.
- Cause: mesh ib 0x119c453800 (96 indices), shading vs 91304408 (ps 34dde251). Shader metas are
  named HashCombine(pgm, perm_idx), binaries <pgm>_<idx>.spv; two separate blobs, binary first.
  20 Oct 08:14 a session rewrote 0x91304408_4.spv (another permutation's code, same as _5) but
  the rename of 0x000009133abab9f5.meta failed (warm-up reading it): the only .meta.tmp in the
  20,378-file cache. Every later start loaded meta(18 Oct spec) + wrong binary.
- Fix (vk_pipeline_serialization.cpp, cache_storage.*; backups *.before-metahash): ShaderMetaVersion
  13 = meta stores XXH3 of the binary written just before it; BinaryMatchesMeta rejects mismatches
  (and removes both files via DataBase::Remove on the IO queue); v12 metas still load when
  DataBase::WrittenTogether (no .tmp, mtimes <= 30 s apart). WriteVector retries the rename 20x5 ms
  and drops the temp on failure. Verified on the original cache copy twice: run 1 rejected exactly
  that pair, floor correct; run 2 no rejections, floor correct (build 5ecdab95).
- Also: per-GPU cache swap (SwapCache, worker-reviewed + LRU fix); GOW3_SKIP_IB_DRAIN knob (MT only,
  default off); invariant positions opt-in (GOW3_INVARIANT_POSITION=1, made no difference).
- Diagnostics added (all env-gated): GOW3_SKIP_FILE accepts index-buffer addresses; depth probe
  modes 3 (GEQUAL) and "4 n" (bias); GOW3_MESH_DUMP=<ib>[+_ALL]; capture "mesh" lines (worker).
  Test scripts: analysis/test-tools/carpettest.sh, carpetlive.sh.

### 21 October 2026: compile-storm leftovers in old shader caches (#92, #95)

- Issue logs: caches built before the storm fix hold thousands of compute pipelines: #92 23,845
  (RX 9070 XT; amdvlk64.dll read fault at 0x60 while warming up, 24,034 of 26,910 done), #95 9,942
  (332 s warm-up), #109 8,282, #74 7,562, #29 7,052, #85 4,484 (89 s). Desktop cache: 8f067540 had
  1,268 cached versions and 7d7e7206 383; no other shader above 35. The storm fix stops new ones but
  the background warm-up still built every old version at every start.
- Fix (vk_pipeline_serialization.cpp FindCrowdedShaders, cache_storage ForEachBlobName): before a
  warm-up reads the store, binary names are counted per program; a program with >= 100 cached
  versions is skipped by both warm-up paths (ReadWarmEntry, LoadPipelineStage) and its binaries,
  metas and c_ pipeline keys (all named by HashCombine(program, index)) are removed through the IO
  queue. On-demand compiles then start that program at index 0 again.
- Verified (build 694e265e, desktop): U2 run 1 removed both (383 + 1,268), 82 compute pipelines
  (was ~1,730), 60 FPS, no storm lines; cache files 6,894 -> 5,245 spv, each storm shader left with
  its 1 new version. U2 run 2: nothing removed, 5,026 keys, 8.8 s warm-up, 60 FPS. U1 and U3 runs:
  82 compute pipelines, 60 FPS, no faults. Backup of the removed files:
  analysis/backups/crowded-before-1021. Laptop package pkg/fix-21oct-7.
- Other issue findings 21 Oct: #102/#107/#94 = U1 0x64011f (CUE pretouch fix); #109 = the same crash
  in U2 (0xd667ff, fill 0xd645f0 + 0x220f as in U1); #105/#74/#29 = 8f067540 storms; #103 =
  AnkerGames repack without files.json; #104 exit 23 = guest debug exception (log needed); #77 movie
  upscale costs 3.3 ms/frame at 4K here (not the cause; open).
- 21 Oct ~13:45: laptop Q2 (U2, NVIDIA GPU, fix-21oct-6, the user playing) stopped at ~16 min on
  image_info.cpp:191 ASSERT(!props.is_block) after "SurfaceFormat: Unknown data_format=36 and
  num_format=13": a garbage T# (BC2 + Ubscaled) with a macro-tiled mode; ImageInfo's constructor
  asserts before PlausibleImage runs. UpdateSize now returns with guest_size 0 for block +
  macro-tiled (never a real texture: it always asserted), PlausibleImage drops it. Build 5cff3a16,
  deployed (backup out/gow3-probe.before-blockmacro.exe = 694e265e), pkg fix-21oct-8; desktop
  U1/U2/U3 regression 60 FPS, no faults. Q2 memory to the stop: RAM 5.1 -> 9.0 GB private, VRAM
  1.7 -> 5.6 GB, arenas 320 -> 1,984 MiB (limit 1,989 MiB, none given back yet), images 1,374 ->
  652 MiB after GC.
- 21 Oct ~14:40, proof of the garbage-T# fix: test switch GOW3_TEST_GARBAGE_TSHARP=<s>[,<n>]
  (vk_rasterizer.cpp TestGarbageTSharp, main bind path, off by default) turns the next n T#s into
  BC2 + number format 13 + a macro-tiled mode. Build without the fix (9d586046) + switch: the exact
  Q2 stop (SurfaceFormat warning, image_info.cpp:191 UpdateSize, STOP). Fixed build: dropped
  ("Ignoring a garbage T#"), but a forced T# in a depth slot then stopped in PromoteFormatToDepth
  (UNREACHABLE for Bc2UnormBlock): ImageInfo/ImageViewInfo now promote only promotable formats
  (IsPromotableToDepth) and give the rest size 0. Final build 8b74cda0 + 3,000 forced: 60 FPS for
  75 s, no stop; U1/U3 regression 60 FPS, shadows correct. Deployed (backup
  out/gow3-probe.before-depthgarbage.exe = 5cff3a16), pkg fix-21oct-9. Test exes:
  analysis/test-tools/bench/exe-garbage-old.exe, exe-garbage-fix2.exe.
- 21 Oct ~16:25, cache swap (laptop T1): no cache went to the wrong GPU (folder profiles: active +
  gpu-2 NVIDIA, gpu-3 AMD). Build 7 (no SwapCache, clears) ran at 13:23 on the NVIDIA GPU and deleted the
  780M cache, leaving two NVIDIA GPU caches; t1c restored the first by folder order. SwapCache now
  restores the newest matching cache (folder stamp) and removes older ones of the same GPU. Desktop
  test (5080 + AMD iGPU, fake older NVIDIA duplicate named gpu-0): iGPU start restored its cache,
  NVIDIA start restored the real one and removed the fake. Build f36c6f02 deployed (backup
  out/gow3-probe.before-swapnewest.exe = 8b74cda0), pkg fix-21oct-10. Cache backup:
  analysis/backups/cache-before-swaptest-1021 (13 GB). User's U3 session 14:5x-15:18 on 8b74cda0:
  ~42 min, median 60, min 57, exit 0.
- 21 Oct evening, new features (deployed 971e6a41, unreleased): surround sound (runtime_audio.c:
  the games mix their 8-channel port for the layout GetPortState reports; port reported stereo so
  only L/R had sound; dumps: told 8 -> all 8 channels active, told 6 -> L R C LFE + surrounds in
  7-8; auto = device channels; launcher Advanced -> Speakers), controller light bar
  (SDL_SetGamepadLED; colours 0000ff/202000/400000), trophies (saved per list in
  user/trophy/trophyNN.txt, unlock state reported, PS5-style popup with the pack's TROPnnn.PNG +
  chime, menu Trophies tab; label 1/2/3 = trophy01/02/03.trp, 54/53/59 trophies; names are in
  encrypted ESFM and the dump has no npbind.dat, so not shown). Tests: services-test (trophy
  logic), GOW3_TEST_TROPHY screenshots in all three games, audio dumps.

## 21 October 2026 (night): upstream 0.5.0 merged

Merge of upstream a77e0c6 (0.5.0) into claude/merge-upstream-0.5.0 (base 6318dfb1, ours 9adda9a).
Decisions (ours = fork kept, theirs = upstream taken):
- Compute IB drain: ours (default on, bit 4) plus upstream's GOW3_SKIP_IB_DRAIN ORed in. Same fix.
- buffer_cache counters, FetchableWindowTick, readback state, GOW3_FETCH_NO_BLOCK: both (independent).
- Frame dump / retexture / readback trigger files and swapchain clear: ours (background watcher, clear only when bars show). Upstream's 250 ms polls and GOW3_PRESENT_SKIP_CLEAR do the same.
- vk_instance: both (present wait plus memory priority / pageable device-local #81).
- Motion eligibility: both, `bound >= 5 || flat_scene || uncharted_gbuffer`. Character motion (stencil_gate) also needs !flat_source, so flat (default) keeps the fork's object motion; velocity mode gets upstream's character vectors.
- Scheduler: both (SubmitLog times around upstream's try_lock; RecordSerial).
- Staging buffer pool: ours (upstream's counter is not in the merged toggles.h; 128 MiB keep budget stays).
- Pending GPU writes (NotePendingGpuWrite, PostDraw): theirs (uncommitted indices, stamps only noted entries) plus our NoteRingWrite and ring epochs. GOW3_PIPE_WRITE_END=0 now also turns off ours.
- SyncSrtTables wait: both, ours first (SrtPendingWrites drain); theirs still guards when bit 512 is set.
- Garbage V# at bind: ours, with ClassifyGarbageSharp using Shader::IsEffectShader. resource.h GetSharp filter: theirs, now gated by Shader::garbage_sharp_filter_on = !Disabled2(SpecGarbageSharps) (bit 1024 used to be ignored by it).
- OnWriteFault, DepthCompareProbe, SkippedShader: combined (our pollers publish upstream's wider modes / mesh lists; timed fault path is theirs).
- FixAimFocusCentre: upstream's write-through signature taken; the ring path passes nullptr (guest copy only, no GPU commands in stage A) and the next direct bind writes the corrected value through. Needs a U2/U3 4K aim check.
- cache_storage / pipeline serialization: theirs (checksum, repair, purge); SwapCache uses our per-game cache_path.
- driver.cpp, equeue, thread.cpp: theirs (HR timer, waits) plus the fork's PresentBeforeFlip / PrepareAtFlipPacket (see the hang below).
- Launcher read_config: ours (v7 = MT on in all games; version stays 8; upstream's FSR 1 migration needs keys the fork lacks). MT row text and probe U1 multithreading: ours (U1 follows the launcher). Speakers, rewards, mouse aim factors, Install from .pkg, drive warning: theirs, auto-merged. Probe speakers block: theirs; its AA comment: dropped.
- JOB_SLEEP_PATCHES: ours (extra 1 ms wait entry, no duplicate signature). runtime_kernel clock_read cache: ours (atomic). runtime_pad read_inject: both (replay= and record tokens).
- tools/install_update.py: TITLE_ID from param.sfo must match [A-Z]{4}\d{5}, so a damaged SFO cannot point the .partial rmtree outside the chosen folder.
- Rejected: upstream's separate Upscaling option and U3 MT-off migration.

Toggle bits: unchanged, upstream added none. Disabled2: 1 FaultPrewait, 2 free, 4 ComputeNestedNoDrain, 8 PresentBeforeFlip, 16 PrepareAtFlipPacket, 32 RingPreciseWrites, 64 RingKeepOnCommands, 128 SrtPipeCheck, 256 DrainCopyShaders (inverted), 512 SrtPendingWrites, 1024 SpecGarbageSharps (now also gates upstream's GetSharp filter), 2048 RangeWritePrepare, 4096 free, 8192 PrewaitAlways, 16384 DlssPrecreate, 32768 FileReadDiscard. First word as in gow3_toggles.h.

Boot hang found after the merge (U3 froze at the start of gameplay, 3 of 3 runs; U1 once stopped before "presenter ready"): thread stacks showed every GPU thread idle, guest threads in kernel_usleep, PresentThread in driver.cpp hr_wait. Env bisection: GOW3_HR_VBLANK=0 alone fixed it (HR_SLEEP, PIPE_WRITE_END, SRT_PENDING_DRAIN, PRESENT_QUEUE, ARENA_RECLAIM, PAGEABLE_MEMORY, COPY_WAIT_BLOCK=0 and MT off did not). Cause: upstream's HR vblank loop waited only for `!requests.empty()`, but the fork presents frames early (PresentBeforeFlip, the presents queue, requests marked presented and completed by CompletePresentedFlips). hr_wait returned at once for an already presented flip, receive_request did not take it, CompletePresentedFlips never ran, so the vblank thread spun in one tick: no vblanks, no flip completions, the guest waited forever. Early presents also never set the wake event. Fix in driver.cpp/.h: hr_wait waits on HasFlipWork(), the HR loop calls CompletePresentedFlips and skips presented flips, new VideoOutDriver::WakePresentThread() is called for early-present pushes and the flip push.

Results: build.sh exit 0; unit tests motion-history, flat-camera, page-state, trigger-files exit 0; python unittests test_prepare, test_game_update, test_apply_update, test_save_backup pass. Boots of 30-45 s with GOW3_FRAME_STATS=1 and HR vblank on: U3 120 FPS, U1 and U2 run. U3 bench, chapter 2 meat market: 76.6 FPS (about 75 before the merge).

Not verified: the single early U1 stall before "presenter ready" (seen once, not reproduced in 4 runs, not confirmed to be the same cause); frame pacing of the new vblank wait against GOW3_HR_VBLANK=0; MT flicker (#23) with the combined pending-write code; U2/U3 aim focus at 4K; velocity-mode character motion; pageable memory on a small card; VRAM trim over a long session; per-GPU cache swap with per-game folders; GOW3_FETCH_NO_BLOCK; the new features in a game (speakers, rewards, trophies, light bar, mouse aim after upstream's rewrite, Install from .pkg); the launcher GUI. No adversarial review of the merge was done (the Codex review was stopped by the user).

## 21 October 2026 (night): DLAA texture LOD bias -1 (toggle bit 4096)

DLSS Programming Guide 3.5 gives the texture LOD bias as log2(render / display) - 1. SceneMipBias() applied it only below native size and kept 0 at DLAA; at DLAA (render >= output, DLSS) it now returns -1 too. Disabled2 bit 4096 (DlaaMipBias, bit set = old 0) is read per draw (Rasterizer::BindSamplers path, sampler cache keyed by the bias bits), so it takes effect live from the toggle file; "Upscaler: scene texture LOD bias" is logged when the value changes. FSR, TAA and inactive keep their values.

Measurements (U3 bar after Continue, 4K, frame dumps by GOW3_DUMP_TRIGGER, Laplacian variance x1e4 of the static wall crop 1650-2030 x 560-820; run-to-run noise ~9%):
- Performance K (bias -2): 13.2 and 14.4 (two runs).
- DLAA K bias 0: 5.3. DLAA K bias -1: 13.4 (+150%).
- DLSS evaluate: 2.07 ms Performance K, 2.28 ms DLAA K, 2.27 ms DLAA K bias -1 (no cost).

Same session, other findings:
- Depth is conventional (near 0.875, far 0.971), so DepthInverted off is right.
- Jitter: 32 phases at Performance, matches the guide.
- The port's `sharpness` (RCAS) does nothing at <= 1; 1.4 gave +20% on the wall crop.
- Model M gave 55% less high frequency than K at Performance and costs +0.85 ms.

Not verified: flicker and moire in motion (the guide warns about them with a negative bias); U1 and U2 at DLAA; cost in heavy scenes (the finer mip raises texture bandwidth); one wall crop in one scene is the only sharpness evidence. Toggle bits: 4096 is no longer free.

## 21 October 2026 (late night): which compute shaders fill the pipe

New diagnostic `GOW3_DISPATCH_STATS=1` (`vk_rasterizer.cpp`): per compute shader it reports count per frame, dims, stage A and stage B microseconds per dispatch, consecutive share and max run. Stage B takes a mutex per packet while it is on.

Setup: U3 4K, repo settings (DLAA M, MT on), stats on, so FPS is ~54 instead of ~67-75. Two runs, last 5 s window of each.

Meat market (backup_ch2meat): 2263 draws + 1894 dispatches per frame; dispatches are 45.6% of pipe items. Stage A dispatch 6.92 ms/frame; stage B dispatch 6.78 ms/frame vs draws 6.15 ms/frame.
- cs 8f067540 (crowd skinning, see the 17 Oct static ranges note): 1529/frame, 1x1x1 in 96%, A 3.7 us + B 3.5 us each (~5.7 ms A + ~5.4 ms B per frame), consecutive 100%, max run 960.
- cs 0c936dd6: 130/frame, 1x1x1 100%, A 4.4 + B 4.3 us, consecutive 99%, max run 130.
- cs 7d7e7206 (cloth skinning): 81/frame, 1x1x1 70%, A 4.9 + B 4.1 us, consecutive 98%.
- cs 2d215c3b: 82/frame, 16x1x1, A 0.8 + B 1.9 us.
- Once a frame: upscaler/scene cs aec539f0 B 169 us; cloth solvers 1c957b01 / 809864d3 B 58 / 37 us.

Chase (backup_ch2chase): 1705 draws + 1581 dispatches per frame (48.1% of pipe items). Dispatch A 6.21 ms, B 6.00 ms vs draws 4.85 ms/frame. 8f067540 runs 1232/frame with the same per-dispatch costs and runs.

Reading:
- One shader issued in back-to-back runs of up to 960 single-group dispatches is ~30% of each stage's time.
- Merging a run into one dispatch (one group per original dispatch, per-group bindings from a table) is the phase-3 candidate of the multi-core roadmap.
- Estimate, not measured: removing ~5.5 ms from both stages moves these views toward the GPU limit.

Not verified: what 8f067540 binds per dispatch (how many buffers, arena vs stream, whether outputs alias), so whether a run can be merged is open. Numbers are with stats on (mutex in stage B, FPS ~54), so absolute us are inflated vs a normal run; only two views in one chapter.

What 8f067540 binds (GOW3_DISPATCH_TRACE=8f067540, new diagnostic in `vk_rasterizer.cpp`; U3 meat market, backup_ch2meat, one captured run of 64 consecutive dispatches):
- Shader: workgroup 1024x1x1, 10 buffers, no images. Res 0-3 formatted reads (stride 8/12/8/8), res 4-5 raw reads, res 6-8 writes (one interleaved vertex stream, stride 28), res 9 flatbuf. `num_records` 0x189 = 393 vertices for the first mesh: one group skins one mesh instance.
- V#s come from a per-dispatch table in memory, pointed to by user data 0 and 2. Only those two dwords change between dispatches, by +0xd0 each time.
- Reads: res 0-4 are the mesh (same guest ranges for consecutive instances of one mesh, e.g. 0x1195625200+3144). Res 5 is 240 bytes of per-instance bones (0x10e08adcd0, -0x13c0 per dispatch). All six reads go through the constant ring (copied by stage A): ~15.9 KB per dispatch, ~24 MB per frame at 1,529 dispatches.
- Writes: res 6-8 go to consecutive, non-overlapping ranges of one 4 GiB GPU buffer (0x1100000000 upward). 64 dispatches wrote 1.83 MB.
- Hazards in the run: write-write 0, read-after-write 0, write-after-read 0 overlaps. Still 54 of 63 dispatches after the first got a barrier (the tracker sees writes to the same host buffer).
- Verdict: in data terms a run can be merged (one shader, regular table stride, no dependencies inside the run). It needs a shader variant that fetches its V#s per workgroup (table base + group id * 0xd0) and reaches buffers by address (the DMA/BDA path), so it is recompiler work. Cheaper steps first: (1) skip barriers between non-overlapping writes in the same buffer (54/63 here); (2) do not re-copy the mesh ranges res 0-4 into the ring for every instance of the same mesh (check why constant-ring reuse does not catch them).
- Not verified: other runs/meshes, other scenes, whether the table stride is always 0xd0, the ring-reuse behaviour for these ranges.

## 22 October 2026: stage B dispatch fast paths (roadmap phase 4)

Profile (prof_b4, U3 meat market, 1080p DLAA K, ~82 FPS): stage B RunDrawPacket 76%; DispatchRecord 53%; BindBuffers 29.6%; ObtainBuffer 20-21%; GpuRangesAdd 10.5-12.7% (boost::icl inserts); CompleteReadbacks 11% (LandGuarded 8.5%); NoteGpuWriteWindows 3.3-4.6% (an unordered_map lookup ~5% self); IsComputeMetaClear 4%; WaitHostCopies 4%. Stage A waits on B 17-22% (DrainDrawPipe); CollectRingBindings 33%.

Changes (Disabled2 bit 524288 DispatchBindFastPaths, bit set = old path, read live; draws take the same paths):
- gpu_modified_ranges is a GpuRangeSet with one pending interval that adjacent or overlapping GpuRangesAdd calls extend. Readers test the set plus the pending interval; ForEachInRange / Count / Subtract flush it first. A joined icl set is canonical, so the result equals eager inserts.
- NoteGpuWriteWindows skips pages already noted at the current tick (ticks only grow; both erase sites clear the memo).
- EnsureResident skips the gap walk for blocks known resident (ReleaseArenaBlocks clears the memo).
- IsComputeMetaClear returns false early when no written buffer is a meta surface: 3 V# reads instead of 9 for cs 8f067540.
- Counters on the GOW3_DISPATCH_STATS line "fast paths per frame" (ranges joined / started / set inserts, page ticks skipped / written, residency memo hits / walks, metadata-clear checks skipped / in full).

Dropped, not equivalent or not redundant:
- CompleteReadbacks / LandGuarded: 98% of samples land real data, and PollReadbacks already returns early.
- SynchronizeMemory skip: needs a CPU-write epoch in the region / page manager.
- Per-program meta-clear memo: the texture cache has no version counter.

Results (ab2.sh live toggle in one session; abstats bootstrap, 95% interval):
- 1080p DLAA K, 6+6 windows: FPS 86.19 on vs 84.04 off, +2.15 [+1.35, +2.92]; median -0.28 ms [-0.35, -0.21]; p99 -0.37 [-1.00, +0.50]; 1% low +0.16 [-7.00, +7.86].
- 4K DLAA K, 4+4 windows: FPS 83.76 vs 81.79, +1.97 [+0.74, +3.30]; median -0.29 ms [-0.40, -0.18]; p99 +0.07 [-2.42, +1.30].
- No hang, image normal.

Not verified: U1 and U2; other U3 chapters; 4K DLAA M (GPU-bound, likely no change); a long soak. p99 and 1% low intervals include 0, so no tail-latency claim. Toggle bits: 131072 and 262144 are taken by work in progress.

## 22 October 2026: multi-core roadmap phases 0-2 and 5

New: `tools/play.py measure` records per game thread CPU time (GetThreadTimes, names from GetThreadDescription) and per logical CPU busy% (NtQuerySystemInformation) in `<tag>.cpu.json`.

Phase 0 baseline (U3, DLAA K, after the stage B fast paths): meat 1080p 88.1 FPS, p99 13.3 ms, 5.47 of 12 logical CPUs; stage A (shadPS4:GpuCommandProcessor) 98%, stage B (gow3:DrawRec) 96%, gow3:VkRecorder 85%, six NdJobWorkerThread 26-28% each. Meat 4K 84.0 FPS, 5.40 cores, 97/94/88%. Chase 1080p 73.0 FPS, 5.37 cores, B 97%, A 95%, recorder 89%. Busy% includes spin waits (GOW3_PIPE_SPIN_US=20 lowered stage B 96 -> 93%).

Phase 1 (env only, meat 1080p, one run each; baselines 88.1 and 87.5):
- GOW3_GUEST_PRIORITY=1: 88.1.
- GOW3_GUEST_AFFINITY=1: 82.8 FPS, p99 21.5 ms (worse; stride 2 maps guest core 6 to logical CPU 12, which does not exist on 12 threads).
- GOW3_PIPE_SPIN_US=20: 87.5.
- GOW3_PREP_WORKERS=2: 88.8.
- GOW3_GPU_THREAD_PRIORITY=above: 88.0.
None beyond noise except affinity (worse). The limit is serial work on stages A/B, not contention.

Phase 2 (placement, timer, priority): not done on this Ryzen 5 5600 (one CCD, 46% of the machine used, phase 1 showed no contention). Only relevant for Intel hybrid or two-CCD Ryzen, not available here.

Phase 5 (stage A, constant ring). Copies within an epoch were already reused; the misses were new epochs (~34,700 submission epochs/s; drains 746/s, in-order tasks 117/s, GPU writes 1,250/s), and 6,953 of 7,016 copies/frame came from pages never CPU-clean.

Change (Disabled2 bit 1048576 RingDurableReuse, set = old path):
- Region manager keeps per-page CPU-modified stamps and change counters (CpuCleanToken / CpuUnchangedSince).
- A range >= 1 KiB copied in 3 epochs in a row gets its pages write-watched by stage A (page_manager watchers, cap 8192 pages). It refuses banned, backing-written, GPU-written and KeepWritable pages.
- InvalidateMemory, the fault path and UnmapMemory drop watches and bump a global ring_watch_gen. A page written twice is never watched again.
- Reuse after the epoch needs an unchanged token.
- Check mode GOW3_RING_REUSE_CHECK=1 compares reused data with guest memory.

Results:
- Per frame 24.4 MB copied before vs 12.9 MB after (reused after their epoch 10.7 MB, in-epoch 12.6 MB).
- 0 of 2,998,257 checked reuses differed.
- CPU writes into watched pages: 5,934 total at start, 0.0 per frame later. 1,313 pages banned; 2,152 watched.
- ab2.sh live A/B, meat market DLAA K, 1080p 6+6 windows: 93.77 vs 86.77 FPS, +7.00 [+5.45, +8.44]; median -0.91 ms [-1.03, -0.77]; p99 -0.78 [-2.07, +0.07].
- 4K 4+4 windows: 90.03 vs 84.67 FPS, +5.36 [+3.81, +6.72]; median -0.77 ms; p99 +0.12 [-0.84, +1.01].
- Thread busy unchanged (A 97%, B 96%, recorder 87%; process 5.66 vs 5.54 cores): same serial threads, more frames.

Open: global ring_watch_gen invalidates every token on any watched-page write (per-page generations if writes appear). Guest mprotect / KeepWritable can open pages without the tracker knowing (same gap as the static arena path). Not tested: U1/U2, other chapters, long soak.

## 22 October 2026: stage A per-dispatch memos (roadmap phase 5b)

Profile (prof_b5, U3 meat market, 1080p DLAA K, stage A = shadPS4:GpuCommandProcessor): GetSharp about 6% (the V# re-reads about 1.1% + 2.6%); RingWatchToken 2.5%; the pending-label scan in SyncSrtTables 2.9%; the second garbage-buffer filter on a V# GetSharp already passed 1.1%. Still there: NotePendingGpuWrite pending_writes scan 3.8%, IsRangeGpuWrittenShared in SyncSrtTables 2.3%.

Change (Disabled2 bit 2097152 DispatchStageAMemos, set = old path, read live):
- Buffer V#s: the specialization (StageSpecialization via PipelineCache::GetProgram) reads each V# of a compute dispatch once and keeps them (PipelineCache::ComputeSharps). The write-range loop in DispatchDirect and CollectRingBindings (through PostDraw) reuse them instead of a GetSharp per buffer and per caller.
- Effect filter: for effect shaders IsGarbageEffectBuffer (in GetSharp) and SpecGarbageFilter apply the same rule to the same V# through the same ClampRangeSize call, so a V# GetSharp returned passes SpecGarbageFilter too and its second call is skipped (Shader::g_buffer_filter_repeats_get_sharp; only while GetSharp runs the full rule, GOW3_EFFECT_SHARP_FILTER not 0).
- Ring watch token: a ring copy asking again for the same range reuses its token while ring_watch_gen has not moved (a watch drop moves it first, so every page is still watched).
- SRT labels: SyncSrtTables skips the pending_labels scan when the read lies outside label_min / label_max, the bounds of every label pushed since the list was last empty (a superset, so a skip is never a missed overlap).
- Counters on the GOW3_DISPATCH_STATS line "stage A memos per frame" (V#s reused / read again, effect filter specializations skipped, ring watch tokens reused / asked, label scans skipped / done).

Equivalence check: GOW3_STAGEA_VERIFY=1 (off by default) also computes the old answer for each cut and counts mismatches on the same stats line (V#s: all 16 bytes compared; watch token: a probe call that adds no watch; labels: the scan runs anyway). One 1080p run (91.4 FPS in verify mode):
- Mismatches: 0 in all four counters.
- Per frame: 1,856 V#s reused and 0 read again; about 1,740 effect filter skips; about 385 ring watch tokens reused and about 385 asked; about 13,300 label scans skipped and about 230 done.

Results (ab2.sh live toggle in one session, 6 rounds x 10 s alternating windows, U3 meat market, DLAA K; bootstrap, 95% interval; 22 Oct):
- 1080p: FPS 95.34 on vs 93.00 off, +2.34 [+1.51, +3.01]; median -0.28 ms [-0.36, -0.19]; p99 -0.27 ms [-0.44, -0.11]; 1% low +3.48 FPS [+0.24, +6.35].
- 4K: FPS 90.75 vs 89.90, +0.86 [+0.30, +1.41]; median -0.11 ms [-0.16, -0.06]; p99 +0.12 ms [-0.62, +0.64] and 1% low -2.09 FPS [-4.04, +0.16] are inside the noise (4K is partly GPU-bound).
- Threads in the verify run: process 5.42 of 12 logical CPUs busy; shadPS4:GpuCommandProcessor 97%, gow3:DrawRec 96%, gow3:VkRecorder 85%. Both serial stages are still the limit, so the roadmap's multi-core gain stays capped by them.

Open (not done):
- Compute key / specialization memo across dispatches. Not exact today: whether each V# is null depends on the per-dispatch bases of resources 5-8 and on the guest mapping. The vma table_generation is static in runtime_memory.c; exporting it would make a memo exact.
- The pending_writes scan in NotePendingGpuWrite (about 3.8%).
- IsRangeGpuWrittenShared in SyncSrtTables (about 2.3%).
- The second ClampRangeSize in CollectRingBindings.

Not verified: U1 and U2; other U3 chapters (one scene measured); a long soak; 4K with DLSS model M. The verify run was 1080p only. p99 and 1% low at 4K include 0, so no tail-latency claim there.

## 22 October 2026: what keeps stage A and B waiting for each other (roadmap phase 6 prep)

All numbers measured 22 Oct on U3 chapter 2 meat market, 1080p, DLAA model K, build 2cd3319 plus the new diagnostic below. One 30 s profile run and one 25 s trace run; no A/B, no behaviour change.

New diagnostic: GOW3_WAIT_TRACE=1 (off by default; liverpool.h / liverpool.cpp, tags at the SendCommand callers). SendCommand / SendFaultCommand take a `tag` (default "other"); the queues hold a QueuedCommand (function plus tag). Every 5 s it prints:
- Per command source: commands run; drains (and how many found the pipe busy) with total microseconds; body microseconds.
- Fence waits: fast path met; slow path drains with microseconds; met; not met.
- Unmet gfx waits by duration, and gfx memory waits by window.

Draw pipe in the profile run: "stage A waited 21.8%, stage B busy 81.4%", about 3,700-3,800 drains/s that waited.
- Drains by reason per second: commands 3,131-3,466 (18.4-18.9% of stage A time); compute 446-463 (1.8-2.3%); rasterizer about 90 (0.7-1.4%); WaitRegMem about 42 (0.3-0.4%).
- GOW3_FRAME_STATS "stage A waits over 0.5%": ProcessCommands:253 9.6%; LandDeferredFenceFor:1186 8.8%; ProcessCompute:2481 2.3%; OnSubmit:4112 1.4%.

Trace run (93.3 FPS with the trace on; six 5 s windows agree within 5%):
- Commands: only one source appears, tag `fault-flush` (the two SendFaultCommand calls in buffer_cache.cpp, guest write faults). About 915-961 runs per window (about 190/s); 1,830-1,922 drains, of which 527-567 found the pipe busy; 440,000-487,000 us waiting (about 9% of stage A); body only 29,000-38,000 us. Average wait per command about 480 us.
- Fence waits (LandDeferredFenceFor): 3,133-3,461 met on the fast path. Slow path: 16,403-17,714 drains (every one found the pipe busy), 457,000-495,000 us (about 9.4% of stage A, about 27 us each), 99.6% met after the drain (first window: 17,648 met, 66 not met). About 3,400 slow-path drains/s, about 38 per frame.
- Unmet gfx waits: 113-151 per window, all under 1 ms (one of 1-5 ms). Gfx memory waits by window: 0x100c100000, 0x100c300000, 0x100c400000 and 0x76f00000 are never unmet; the unmet ones sit at 0x100db00000-0x100dc00000 and 0x100e100000-0x100e700000 (about 1,700-1,950 unmet per window).

Reading:
- Both stages wait on each other in turn. Stage A spends about 18.5% of its time in two drains-to-empty (fault-flush commands 9.1%, deferred-fence waits 9.4%), and stage B sits idle while stage A feeds it again (DrawPipe::Run self 18.7% plus ntdll 15.2% in the stage B profile).
- The fence drains almost always end with the fence met, so they wait for stage B to drain everything queued, not for a GPU result.
- The fault-flush commands read and change cache state stage B also uses, so their drains are needed as they are.
- Possible cut, not done: stage B lands the fence tick as an in-order pipe task and stage A waits for that task only, not for the whole queue. Not safe by a plain change: stage B owns the readbacks while it runs (the 18 Oct race, GOW3_READBACK_RACE), so stage A must not land ticks beside it.
- Upper bound if both waits vanished entirely: stage A 21.8% shorter. The real gain is smaller, because the work before the fence still has to finish.

Profile after 5b (symbolized with symprof_k.py / children_k.py; GPU-command-thread window 4 and draw-recording-thread window 4, 1,744 and 1,747 samples):
- Stage A inclusive: ProcessCompute 61.8%; DispatchDirect 46.8%; PostDraw 38.4%; CollectRingBindings 33.4% (CopySparseMemory 15.1%); ProcessGraphics 29.6%; DrainDrawPipe 21.7%; GetComputePipeline 13.2% (RefreshComputeKey 12.8%: Info::RefreshFlatBuf 6.9%, StageSpecialization ctor 6.8%, ForEachBufferSharp 5.4%, GetSharp 3.1%); LandDeferredFenceFor 9.0%; SyncSrtTables 6.0%; NotePendingGpuWrite 5.1%. Self: ucrtbase 19.8% (memcpy of the ring copies), ntdll 19.0% (waits).
- Stage B inclusive: RunDrawPacket 76.1%; DispatchRecord 50.3%; BindResources 48.4%; BindBuffers 27.0% (ObtainBuffer 14.7%, self 5.2%, IsCpuWriteHot 1.9%, IsBufferAccessed 1.8%); ObtainBuffer 16.7% (GpuRangesAdd 7.8%, which holds NoteGpuWriteWindows 4.1%, of which the gpu_write_page_tick unordered_map 3.5%; SynchronizeMemory 3.8%); CompleteReadbacks 12.9% (LandGuarded 9.3%: WriteBackGpuData 4.3%, TryReadBacking 2.1%). Self: DrawPipe::Run 18.7% and ntdll 15.2% (waiting for work).
- gow3:VkRecorder (profile prof_b5, window 4): 53.5% self in Scheduler::RecorderThread (its wait loop), nvoglv64.dll 21.2%, ntdll 18.2%, DispatchRecord 10.4% inclusive. Not the limit.

Next candidates by size (not done):
- Stage B gpu_write_page_tick map (about 3.5% of B).
- Stage A NotePendingGpuWrite scan (about 5% of A).
- Stage A cross-dispatch compute key memo (RefreshComputeKey 12.8% of A). Needs the vma table_generation exported from runtime_memory.c.
- The fence-tick-as-pipe-task idea above (up to about 9% of A, concurrency risk).

Not verified: other scenes and chapters; U1 and U2; 4K; each number is from one run. The "entered busy" count stands in for the pipe queue depth (DrawPipe has no cheap item count). The overhead of the trace was not measured.

## 22 October 2026: page tick write-back cache (roadmap phase 6a, stage B)

Profile after phase 5b (U3 meat market, 1080p DLAA K, stage B = gow3:DrawRec): ObtainBuffer 16.7%; GpuRangesAdd 7.8%, which holds NoteGpuWriteWindows 4.1%, of which the gpu_write_page_tick unordered_map find/assign 3.5%. cs 8f067540 (~1,530 dispatches a frame) writes about 3 pages per dispatch, mostly the same pages every frame.

Change (Disabled2 bit 4194304 PageTickCache, set = old path, read live; `buffer_cache.cpp`: BufferCache::NotePageTick / PageTick / ErasePageTick / FlushPageTicks):
- Small GPU writes now go to a direct-mapped write-back cache of 16,384 pages in front of gpu_write_page_tick (256 KiB, slot = page ^ (page >> 14), 16 bytes: page and tick).
- An occupied slot holds the page's newest tick (the map may hold an older one or none) and is always dirty, so it needs no flag. A write to the same page updates the slot; a write to another page first writes the occupant into the map, then takes the slot.
- Readers (the download-reason bits in ReadMemory, FetchableWindowTick, PrewaitPageWrites) check the slot first, then the map. ErasePageTick drops the slot and the map entry (DiscardCpuWritePage). SweepTracking runs FlushPageTicks before it looks at the map, so the sweep sees every page's newest tick.
- Locking: everything under gpu_ranges_mutex. Writes, erases and flushes hold it exclusively; readers shared or exclusive (slots change only under the exclusive lock). The slot array is plain memory, the counters are atomics.
- With the bit set, the first NotePageTick flushes the cache into the map and the old path runs from then on, so the toggle is safe live. The phase 5b memo (page_tick_memo) still skips pages already noted at the current tick before the cache is asked.

Check mode: GOW3_PAGETICK_VERIFY=1 (off by default) also writes every tick into the map and compares each cached read with the map. One 1080p run (92.6 FPS in verify mode): 0 mismatches.

Counters on the "fast paths per frame" line of GOW3_DISPATCH_STATS (cache write hits / write-backs / reader hits / flushes / mismatches), from that run:
- Per frame: 4,500 cache write hits of 4,670 page ticks written (the memo skipped 13,350 more), about 104 write-backs, 1 reader hit.
- Flushes: 2-3 per 2 s line.

Results (ab2.sh live toggle in one session, 6 rounds x 10 s alternating windows each side, U3 chapter 2 meat market, DLAA K; bootstrap, 95% interval; 22 Oct):
- 1080p: FPS 96.81 on vs 95.88 off, +0.94 [+0.47, +1.43]; median -0.07 ms [-0.12, -0.02]; p99 -0.16 ms [-0.37, +0.02]; 1% low +0.96 FPS [-2.83, +4.71].
- 4K: FPS 92.28 vs 91.28, +1.00 [+0.62, +1.38]; median -0.15 ms [-0.22, -0.08]; p99 -0.08 ms [-0.30, +0.11]; 1% low +0.45 FPS [-1.23, +2.20].

Not verified: U1 and U2; other U3 chapters (one scene measured); a long soak; 4K with DLSS model M. The reader paths ran only about once a frame in the verify run, so the check covers the write path far more than the three reader sites. The verify run was 1080p only. p99 and 1% low intervals at both resolutions include 0, so no tail-latency claim.

## 22 October 2026: fence wait for its packet's position instead of a full drain (tried, not kept)

All numbers measured 22 Oct on U3 chapter 2 meat market, 1080p, DLAA model K, build 6faf3ff plus the change below. The code is not committed: the diff is saved outside the repository as `C:\dev\bench\fence_position_wait.patch` (13.7 KB; `git apply` it on 6faf3ff to retry). It touched liverpool.cpp / .h, vk_draw_pipe.h, vk_rasterizer.cpp / .h and gow3_toggles.h.

Idea: on its slow path Liverpool::LandDeferredFenceFor (liverpool.cpp) drains the whole draw pipe (about 3,400 a second, 9.4% of stage A in the wait trace, 99.6% ending with the fence met; see the section above). Tried: when the label has a queued write in pending_fences, stage A waits only until stage B has run that packet (DrawPipe::WaitReached(position), position of the newest covering pending entry). Then it reads the deferred value (BufferCache::DeferredFenceTick, under deferred_values_mutex) and passes if the wait is met. Anything else falls back to the full drain.
- Toggle: Disabled2 bit 8388608 FencePositionWait (set = old path).
- Check mode: GOW3_FENCE_VERIFY=1 (off by default) also does the old full drain and compares the value.
- Counters: on the GOW3_WAIT_TRACE fence line.
- Stricter than GOW3_FEWER_DRAINS=1 (no wait at all), which made a multithreaded readback race frequent (2 of 5 multithreaded U3 runs stopped flipping, see the FEWER_DRAINS notes above in this file).

Check run (GOW3_FENCE_VERIFY=1, 30 s, 79.9 FPS in verify mode):
- Verify mismatches: 0.
- Per 5 s window: 3,400-16,300 position waits; 99.5% met after the position wait; 0.2-0.5% fell back to the drain.

Why it was dropped: the wait itself barely got shorter.
- Average position wait 21.8-24.6 us (three windows) against 26.7 us for the old full drain: about 14% shorter.
- That is at most about 1.3% of stage A time (about 0.5-1 FPS). The fence's packet is nearly always close to the head of the queue, and stage B's latency to finish it is what stage A waits for.
- Under the 1 FPS threshold set for this series.
- The path keeps stage B busy while stage A goes on, which is the window the FEWER_DRAINS race needs, so it would need a long multithreaded soak first.

The 1080p live-toggle A/B (6+6 windows) is NOT usable: it ran at 74.9 vs 73.8 FPS (instead of the usual 93-96) with a difference of +1.12 [-7.92, +10.84] FPS. The machine was loaded by other programs (Chrome about 1.2 cores, CPU load 46-82% with the game stopped).

What would still cut the fence and fault-flush waits (not done):
- Make stage B finish packets faster (what the cuts so far did).
- Have stage B do the fence check as part of the packet, so stage A does not wait at all. Needs the same race analysis as above.
- Fault-flush commands (about 9% of stage A, 190 a second, about 480 us each) were not touched.

Not verified: no 4K run, no soak run, no GOW3_READBACK_RACE run; U1 and U2; other U3 chapters (one scene measured); each number is from one run.

## 24 October 2026: CpuCleanToken re-checks the change counter

Mechanism: `RegionManager::CpuCleanToken` read `cpu_changes_done` and `cpu_changes_started` (equal), then the CPU bits, and returned `started + 1` without reading `started` again (a seqlock without its second check). Stage B starts a change (started = n+1, `cpu.UnsetRange(P)`, protection not yet applied). Stage A reads the bits of P as clean and gets token n+1. A guest store to P lands after the ring copy (page still writable: no fault, no StampSet). Stage B then protects P and sets done = n+1. `CpuUnsetSince(stamp < token)` later passes and the stale ring copy is reused. Only sets leave a stamp, clears do not.

Fix: after `cpu.AnyInRange`, `std::atomic_thread_fence(std::memory_order_acquire)`, then reload `cpu_changes_started` and return 0 if it differs from the first read (this also covers a change that started and finished in between). The fence is needed because the bit reads are not atomic loads of that member. No other behaviour change.

Not verified: no game run; GOW3_RING_REUSE_CHECK=1 was not run after the change; race never observed (found by static review).

## 24 October 2026: DiscardCpuWritePage clears gpu_add_cache

Mechanism: `GpuRangesAdd` returns early on a `gpu_add_cache` hit (same address and size) without adding to `gpu_modified_ranges`. `GpuRangesSubtract` clears the entries overlapping the range, but `DiscardCpuWritePage` (21 Oct file-read path) called `gpu_modified_ranges.Subtract(page, 4096)` directly and left them. A later GPU write with the same address and size hit the stale entry, was not added back, and later downloads of that page skipped the GPU bytes. These were the only two Subtract call sites.

Fix: the entry-clearing loop of `GpuRangesSubtract` is now `GpuAddCacheDrop(address, size)` (caller holds `gpu_ranges_mutex`). `GpuRangesSubtract` calls it after Subtract; `DiscardCpuWritePage` calls it with `(page, 4096)` right after its Subtract, inside the same lock block. No other behaviour change.

Not verified: no game run; never observed (found by static review).

## 24 October 2026: DMA_DATA skip only for address destinations

Mechanism: commit e1df90a replaced `ASSERT(das == 0)` with a skip of every DMA_DATA packet whose `command.das` is set (RunDmaData, the graphics caller's NotePendingGpuWrite guard, the compute queue case). Per the AMD PM4 spec DAS (destination address space: memory or register) only applies when the destination is an address (dst_sel Memory or MemoryUsingL2). With dst_sel Gds it has no meaning and the packet must run, but the skip dropped such a GDS write silently.

Fix: file-local `DmaDstIsRegister(d)` in liverpool.cpp returns `das != 0 && dst_sel in {Memory, MemoryUsingL2}`. RunDmaData and the compute case skip on it; the graphics caller's guard is `!DmaDstIsRegister(...)`. The once-only log now prints dst_sel too. Packets with dst_sel Gds run as before e1df90a. No other behaviour change.

Not verified: no game run; no Uncharted scene known to send das=1.

## 24 October 2026: compute DMA_DATA waits for host copies

Mechanism: the graphics RunDmaData and the compute WriteData call `WaitHostCopies()` and `WaitDeferredSignals()` before they write guest memory. The compute-queue `case PM4ItOpcode::DmaData:` (Liverpool::ProcessCompute) did not: after the DmaDstIsRegister skip it ran the FillBuffer/CopyBuffer CPU paths on stage A at once. (a) An earlier dispatch's deferred guest->staging upload copy (recorder thread) could run after this DMA wrote the range, so that dispatch uploaded the post-DMA bytes. (b) A pipelined RELEASE_MEM label deferred to the recorder could be overtaken by this guest-visible write.

Fix: `WaitHostCopies(); WaitDeferredSignals();` before the three branches that write guest memory with a Memory/MemoryUsingL2 destination: Data -> Memory, Gds -> Memory, and the non-patch CopyBuffer of Memory -> Memory. Not added: the Gds-destination branches (no guest write), the `indirect_patches.emplace_back` path (no guest write; a wait there would stall every dispatch), NoteOvertake (graphics-only helper). No other behaviour change.

Not verified: no game run; not known how often U3 sends compute DMA_DATA to memory; never observed (found by static review).

Reviewed, not changed (24 Oct 2026). Each needs a game run (GOW3_UPSCALER_TRACE / boot.log) or a shader-cache version bump before changing:
1. FMask T# makes the resource list depend on the T# (resource_patching_pass.cpp:294-341, `descriptors.Add` skipped).
2. The effect-family garbage rule nulls unbounded raw V#s (resource.h:116-134).
3. BufferSpecialization lacks add_tid_enable.
4. access_extent comes from permutation 0.
5. Baked stride for a null-filtered slot.
6. DumpConstRam memcpy without WaitHostCopies (liverpool.cpp:1418).
7. Ring copies vs pending GDS stores / compute labels (vk_rasterizer.cpp:2051).
8. No history reset after movie/load frames without a frame start, no camera-cut detection (vk_camera_motion.cpp:442).
9. World-space single-quad draws get no jitter (vk_rasterizer.cpp:3445).

## 25 October 2026: session recorder

Why: bug reports from players arrive as a sentence and maybe a screenshot. Everything needed to
read one (build, settings, log around the moment, memory growth, exit code) existed but was
scattered over the install folder and overwritten by the next launch. `tools/session_recorder.py`
(started with `Record-Session.cmd`; both are shipped in the package) collects it per run.

**Design.** A separate stdlib-only process, below-normal priority, that never opens the game's
log for writing, never reads the game's memory and never injects. It polls for `Uncharted.exe`,
records one session, finalizes it when the process ends and waits for the next launch. The
install root and the `out` dir come from the game's own command line (`<root>\analysis\<name>\boot-linked.bin`,
read once through `Get-CimInstance Win32_Process`), so it works against an older installed
package. `--root`, `--out-dir`, `--sessions-dir` override. Folder:
`<root>\analysis\uncharted-user\sessions\<YYYYmmdd-HHMMSS>_<out dir name>\` (the time is the
process creation time, so a recorder started late still names the run correctly).

| File | What |
| --- | --- |
| `session.json` | Recorder version, start (local + UTC), Windows, CPU (registry), RAM, GPU (`nvidia-smi` csv with max clocks and power limit; `Win32_VideoController` if missing), resolution, power plan, game exe path/size/mtime/sha256, `VERSION`, git HEAD (reads `.git\HEAD` only), the process command line, `launcher.json`, the recorder's own `GOW3_*`/`UNCHARTED_*` variables. Written in stages, usable after each. |
| `config\` | Copies of `probe-u1/u2/u3.ini`, `test-env.txt`, `disabled-optimizations.txt`. |
| `boot.timed.log` | `boot.log` polled every 0.5 s, each new complete line prefixed `HH:MM:SS.mmm`. A shrinking file restarts from 0. Lines already in the log when the recorder attached are stamped `--:--:--.---`. |
| `events.txt` | Lines matching warning/error/assert/unsupported/fail/Dropped/garbage/skipped/corrupt/mismatch/Hang watchdog/Texture cache: memory/first unsupported/exit code, plus Frame stats windows with a worst frame over 40 ms or any shader/pipeline compile. An identical line is written 20 times, then counted. |
| `metrics.csv` | Every 2 s: process CPU % (of all cores) and cores busy (`GetProcessTimes` delta), working set, private bytes, threads (Toolhelp snapshot), handles, free system RAM, GPU util/memory/clock/temp/power (one long-running `nvidia-smi -lms 2000` child, several GPUs: the one with most memory in use), foreground window title. |
| `marks\NN_<label>_<time>\` | `mark.json` (label, time, elapsed since process start, log byte offset and line number, last Frame stats line, latest metrics row, foreground window, game window rectangle, what the screenshot and the dump did), `log_tail.txt` (last 300 timed lines), `screen.png` (GDI `BitBlt` of the game window's rectangle from the screen DC; an all-black result is retried once from the whole primary screen and `mark.json` says so; PNG written by hand with zlib), `engine_dump\` (see below). |
| `markers.log` | One line per mark. |
| `boot.log`, `boot.json`, `frametimes.txt`, `framelog.txt`, `launcher-last-run.log` | Final copies at the end (skipped with a note when older than the session). |
| `hang\` | `hang-*.txt` reports of the run; `.dmp` files are hundreds of MB and are only named in `summary.md`. |
| `windows_events.txt` | Application-log events mentioning Uncharted in the session window (`wevtutil`). |
| `summary.md` | Duration, exit code (0xC0000005-style statuses flagged, `1` noted as what STOP/taskkill leaves), marks, FPS min/median/avg and worst frame (counts are of 5 s windows, the log keeps only each window's worst frame), shader compiles, average/peak of the metrics, top repeated warnings, notes. |
| `recorder.log` | Only created when a worker raised; every worker and finalize step is wrapped so one failure never loses the others. Files are flushed after each write, so a killed recorder leaves usable files. |

**Hotkeys.** `RegisterHotKey` on a dedicated thread with its own message loop (`MOD_NOREPEAT`);
default F7 texture, F8 geometry, F9 flash, F10 other, `--keys "F7=texture,..."` changes them.
A key another application owns is reported and the others still work. The press runs in a worker
thread (a mark takes seconds), the same key is ignored for 3 s. `winsound.Beep` gives feedback
because the game is fullscreen: one short = captured, two = engine dump done too, low long = failed.
Engine dump: if `<out>\dumps` exists (the game started with diagnostics) the recorder creates
`<out>\dumps\trigger`; the presenter takes it (`GOW3_FRAME_DUMP_TRIGGER`, vk_presenter.cpp) and
writes the next frame's guest display buffer and presented image as `name_index_WxH.raw`
(32-bit pixels) to `GOW3_DUMP_DIR`. The recorder waits up to 4 s for new files, then until their
sizes stop changing (the readback is deferred), moves them into the mark and removes a trigger the
game did not take. Limits: `--max-engine-dumps` (20), `--no-engine-dump`, and less than 2 GB free on
the session drive skips screenshots and dumps (noted in `mark.json`). `boot.timed.log` and
`events.txt` stop at 200 MB each with a note. The recorder sets the console out of QuickEdit
mode (a click in the window would otherwise block its printing).

**Limits.**
- No per-thread CPU: one `GetProcessTimes` delta for the whole process, to stay under 1 % of a core.
- The game's own environment cannot be read from outside. `session.json` holds the recorder's variables and says so; what the game got follows from `config\` plus `test-env.txt`.
- `boot.log` has no times: lines are stamped when seen, so +-0.5 s. A game that was already running when the recorder started has its earlier lines marked unknown.
- The GDI screenshot can be black or stale for exclusive-fullscreen / flip-model Vulkan windows; then the engine dump is the reliable image. The raw dump is not a PNG.
- The hotkeys are global and are taken from whatever has focus, so they clash with game or overlay keys (change with `--keys`).
- A dump request makes the presenter copy two images to the host: expect one hitch of a frame or two on a mark.
- Only the first `Uncharted.exe` found is recorded; sessions are sequential.

**Tests.** `python tools/session_recorder.py --selftest` (temp fake root; a dummy `python` child that exits with code 3 stands in for the game; a fake presenter thread answers the trigger; the hotkey registration is skipped and the mark routine is called directly) and `gow3_pc/tests/test_session_recorder.py` (line classification, Frame stats parsing, duplicate collapsing, log tailing with partial lines and truncation, PNG encoding, BGRA conversion, command-line parsing, exit-code text, summary). Both ran green on the development machine (Windows 11, RTX 5070, `nvidia-smi` present: its field names were checked against the real tool).

**Not verified.** No real game run: the hotkeys over a fullscreen Vulkan window (focus, whether
`RegisterHotKey` keys arrive while the game has the keyboard), the screenshot of a real game
window (black or not, DPI, borderless vs exclusive), a real engine dump through the trigger
(only a fake presenter answered), the command-line parse against a real `Uncharted.exe` command
line (tested on strings shaped like it), `boot.json` timing at a real exit, the overhead on the
game (not measured; the design budget is one `GetProcessTimes`/Toolhelp pass every 2 s plus a
0.5 s file poll), long-session behaviour, a killed game (STOP) and a crashed one (exit code
from the handle), multi-GPU machines, non-NVIDIA machines (the `Win32_VideoController` fallback
was not run), localized Windows beyond the OEM-codepage decode of tool output.

## Reflex and DLSS Frame Generation (22 Oct)

Decisions.
- Reflex goes through VK_NV_low_latency2 directly (no Streamline, no NVAPI). Driver 616.92 reports revision 2, which attributes submits between presents to the later present and ignores VkLatencySubmissionPresentIdNV. The sleep is before the first pad read of each guest frame; markers are keyed on the flip ordinal.
- Frame Generation goes through NGX directly in the MIT bridge (ABI 3), not Streamline. Streamline's DLSS-G guide section 8.0 requires sl.reflex and rejects other Reflex integrations; NGX-direct does not require Reflex (the DLSS SDK FG guide only says FG "works with" Reflex).
- Menu and launcher: reflex (Off / On / On + Boost) in the Performance tab/page, frame_gen (Off / On) under anti-aliasing / Display. The launcher adds nvngx_dlssg.dll like nvngx_dlss.dll (signature check); the package refuses it, apply_update.py CARRY and update_port.sh carry it over for the player.

Reflex measurement. U3 chapter 2 save (backup_ch2meat), 4K DLAA, RTX 5070, live A/B, 6+6 windows of 10 s, 95% hierarchical bootstrap.
- reflex 1 FPS 44.1 vs 53.5: diff -9.5 [-12.4, -6.7]. Median frame +2.9 ms [+1.5, +4.3]. (First round, 6+6 windows; superseded for the FPS cost by the corrected round below, 57.2 -> 44.0.)
- CORRECTED (22 Oct, second round): this first round also said "driver timing latency, input sample to GPU render end: +1.7 ms [-2.2, +5.7] (no gain)", repeated in CHANGELOG and commit 8427816. That was wrong. The number came from joining the driver's timestamps (vkGetLatencyTimingsNV) to the host clock, which does not line up (a 288 s log against the windows; frames landed in the wrong windows). With the state written in every log row (below) Reflex does lower the latency. Flip ids stayed paired (lag 0).
- PresentMon 2.6 reports no PC latency (InstrumentedLatency NA) for VK_NV_low_latency2, so latency comes from vkGetLatencyTimingsNV (GOW3_REFLEX_LOG).
- The earlier cause ("the port's limit is its own CPU stages, which the driver's pacing model does not see") is replaced by the GPU-saturation explanation in the subsection below.

### Reflex, corrected measurement (22 Oct, second round)

Setup: RTX 5070, driver 616.92, U3 chapter 2 "meat market" save, DLAA K. Live A/B by toggle bit inside one session. Latency = driver timing "input sample to end of GPU work" from vkGetLatencyTimingsNV; every frame is labelled with its state in GOW3_REFLEX_LOG (15th column `state`: 0 off, 1 on with sleep, 2 on without sleep). 95% bootstrap with the state runs (windows) as units. 9 game sessions this round (one discarded: the game started late and the windows measured loading).

4K DLAA (measured, live A/B, bootstrap on windows):

| Setting | FPS | Latency change, mean | median | p95 |
|---|---|---|---|---|
| Off | 57.2 (port stdout 58.5) | - | - | - |
| Reflex On | 44.0 (stdout 46); stdout 58.5 -> 46 is -21.4%, log 57.2 -> 44.0 is -23% | -10.6 ms [-11.0, -9.8] | -9.3 [-9.6, -8.9] | -3.2 [-3.9, -2.9] |
| Reflex On + Boost | 48.6 (off 59.2 in that pair), -18%; FPS from the port's own frame count (play.py measure stdout, windows of 10 s), not from the driver log (log FPS for that session not computed); latency cells are log-derived | -9.9 ms [-10.3, -9.3] | -7.6 ms [-7.7, -7.6] | -2.0 ms [-3.4, -1.5] |

Each latency cell is the difference (state minus off) of that statistic (mean, median, p95) of input to GPU end, with the state runs as clusters (2 runs per state, 150 frames dropped at each state change for the Boost row). The FPS percentages come from two sources and are not mixed: port stdout (58.5 off, 46 on, -21.4%) and the PresentMon-derived log (57.2 off, 44.0 on, -23%). The Boost pair (59.2 -> 48.6, -18%) is a port count (stdout), not log-derived.

- Equal frame rate (measured, plain 45 FPS limit without Reflex against Reflex On): input to GPU end about 54 ms against about 34 ms, so about 20 ms better at the same FPS.
- Input to GPU start (measured): 27 -> 15 ms.

1080p DLAA (measured, CPU bound, about 95 FPS):

| Setting | FPS | Latency |
|---|---|---|
| Off | 94.8 | input to GPU end about 14 ms |
| Reflex On | 94.1 (cost under 1%) | about 14 ms; difference +1.2 ms [-0.4, +2.1], none |

So Reflex only matters, and only costs, when the GPU limits the frame rate.

Why it costs FPS. Measured: at 4K DLAA with Reflex off the GPU takes 16.9 ms per frame with no idle gaps (FPS-off period 16.8 ms); with Reflex On the GPU has idle gaps (p95 8.2 ms) and input to GPU start falls from 27 to 15 ms. The cost is entirely the sleep: Reflex mode plus markers without the sleep (toggle bit ReflexNoSleep 33554432) gave 58.6 FPS, the same as off (58.5).
Inference, not proven: the driver's pacing leaves the GPU starved in part of the frames because of jitter in the CPU stages. No test isolated this mechanism (for example by removing the jitter); it only fits the numbers above.

Tried, no recovery (measured):
- GOW3_REFLEX_VARIANT=3 (render-submit span about 0): 41-46 FPS, mean sleep 17.5 ms, worse.
- GOW3_REFLEX_VARIANT=2 (chained from the previous frame): 45-48 FPS.
- GOW3_FRAMES_AHEAD=2: 43.8-49.4 FPS.
- Boost: -18% (port count, stdout) instead of -21.4% without Boost (stdout); the log-derived -23% has no Boost counterpart.

Marker attribution (driver revision 2): no queue is marked out-of-band; the shared graphics queue carries all game submits. An agent claim that marker ids must match VkPresentIdKHR is wrong per the spec (they are unrelated). Default stays off.

### Frame limit and pacing with Frame Generation (22 Oct, second round)

Frame limit (code change): with Frame Generation on, the limit counts shown frames and may be 144, 160, 165, 180, 200 or 240 (the menu lists them only when Frame Generation is supported); without it the limit stays at most 120. Before, a shown-frame limit of 120 held real frames to 60 FPS.

Pacing of the generated frame, 1080p DLAA K, shown-frame limit 160 on a 165 Hz display, 3+3 windows (measured with PresentMon DisplayedTime and the port's FrameGen stats line):

| Variant | Real FPS | DisplayedTime | Generated -> real gap |
|---|---|---|---|
| High-resolution timer (default now) | 80.0 | 96-97% of intervals exactly one refresh (6.06 ms), about 3.0-3.3% two refreshes | 6.5 ms (target 6.25) |
| std::this_thread::sleep_until (toggle bit FrameGenStdSleep 67108864) | 80.0 | 96-97% one refresh, about 2.9-3.1% two refreshes | 7.2-7.5 ms (rounded to ms) |

- The two-refresh intervals are the 160/165 beat (5 of every 165 refreshes repeat); the timer type made no difference there.
- FG off, same scene: 84-90 FPS, intervals quantized to 12.12 ms (measured).
- The new `FrameGen:` stats line prints every 5 s.

Rejected experiment, removed: a deadline pacer for FG (absolute incremental deadlines, period filtered from arrival intervals). Real FPS fell from about 80 to 52-65 and 37-48% of displayed intervals were above 1.5x the median. Cause (read from its stats: period 19.3 ms, wait 17.7 ms per frame): the period is estimated from arrival intervals that the pacer itself shapes, so it can only ratchet up. A feedback on slack (frame ready earlier than its deadline) would be needed; not tried.

The generated present is wrapped in out-of-band present markers (VK_NV_low_latency2) with the real frame's id so the driver does not count it as a new frame. This was implemented from the spec; the short run below ran both on at 1080p and did not validate it.

### Reflex + Frame Generation, one short run (22 Oct 2026)

Setup: U3 chapter 2, 1080p DLAA, GOW3_FRAME_GEN=1, GOW3_REFLEX=1, shown-frame limit 160, 3 windows of 10 s, no state toggling.

Measured:
- Real 78.1-78.9 FPS (port count). About 80.0 with FG and Reflex off in the earlier run, so about -1.5%.
- Reflex stats lines: sleeps avg 0.09-0.14 ms, 0 timeouts, lag=1 (guest flips minus prepared flips; 0 in the 4K runs without FG; not investigated).
- FG stats: gen == real, no skips, gap_ms mean 6.55-6.69, eval_ms mean 0.42-0.49.
- Driver log (15 columns): marker order correct in 100% of rows (simStart<=simEnd, renderSubmitStart<=End, presentStart<=End, renderSubmitEnd<=presentEnd). 15% of rows have no input sample (868 of 5605; similar to the 12% of the FG-off 1080p log, 1957 of 16808). 6.6% of rows have no GPU times (371; 0 in the FG-off log).

Problem found: the driver's GPU range per frame is much shorter with FG (median 1.8 ms) than without (10.3 ms), and input-to-GPU-end is 33.5 ms with FG vs 13.3 ms without. Under FG the driver's per-frame accounting does not describe the frame's real GPU work: the out-of-band present markers did NOT make the attribution coherent (or the generated present's work is split in a way the log does not show). Latency numbers with FG on are therefore not interpretable as end-to-end latency. Reflex pacing under FG at 4K (where it sleeps ~10 ms) remains untested and possibly mis-paced.

State: stable and cheap at 1080p, attribution not validated, 4K untested.

Not verified in this round:
- Frame Generation together with Reflex at 4K, and the attribution of the driver's per-frame accounting under FG (see the short run above).
- Generated-frame image quality.
- Frame Generation at 4K (the pacing numbers are 1080p only).
- VRR/G-SYNC state of the display during the tests: unconfirmed. DisplayedTime was quantized to the 165 Hz refresh, which fits a fixed refresh but does not prove VRR was off. The two-refresh share (the 160/165 beat) is only valid for a fixed refresh.
- U1, U2 and other U3 chapters (one scene only).
- Reflex marker attribution inside the driver (only the revision 2 behaviour above).

Frame Generation smoke run (same scene, DLSS DLAA, window 2560x1440).
- Feature created at window size with depth/motion 3840x2160. Real 48.5 FPS, displayed 98.1 FPS (PresentMon 978 presents vs 485 real). No NGX errors.
- Without nvngx_dlssg.dll: no FG, presents equal real frames.

Not verified / open: generated-frame image quality; hudless is not passed (the HUD may ghost; the upscaled scene precedes the game's post/tonemap, so it is not a valid hudless); U2's camera view x points to screen left (NGX documents right); TAA and FSR paths; resize and HDR; live toggling; latency with FG; U1, U2 and other chapters; long soak; GPUs without FG; ring memory about 100 MB per slot at 4K.

Toggle bits: ReflexSleep 8388608, FrameGeneration 16777216, ReflexNoSleep 33554432, FrameGenStdSleep 67108864. Settings and env: reflex / GOW3_REFLEX, GOW3_REFLEX_EXT=0, GOW3_REFLEX_LOG (15th column `state`), GOW3_REFLEX_VARIANT (diagnostic, 2/3); frame_gen / GOW3_FRAME_GEN, GOW3_FRAME_GEN_EXT=0, GOW3_NGX_LOG.

## Reflex: GPU idle diagnosis and early submit (22 Oct 2026)

Setup (all numbers measured 22 Oct): U3 chapter 2 Mercado de Carne, 4K DLAA, RTX 5070, frame generation off, reflex 1, GOW3_GPU_PROFILE=1. Toggle files switch the states live inside one session; windows of 15 s (30 s in the diagnosis session), 2 per state, no confidence intervals, one session each.

### Diagnosis: where the Reflex frame rate goes

Diagnosis session, 4 windows of 30 s in the order on, off, off, on:

| Measure | Reflex on | Reflex off (toggle ReflexSleep set; the log shows mode 0) |
|---|---|---|
| FPS, two windows | 53.5 and 52.6 | 63.7 and 65.1 (about -19% with Reflex on) |
| GPU time per frame (profiler) | 18.9 ms | 15.65 ms |
| DLSS evaluate | 8.0 ms | 8.2 ms |
| GPU clock mean, two windows | 2955 / 2956 MHz | 2932 / 2926 MHz |
| GPU utilization | 79% | 96-97% |
| Power | 172 W | 204 W |
| "(between submissions: GPU idle)", total | 4.12 ms/frame | 0.48 ms/frame |
| of which at Process loop -> Rasterizer::Flush (sceGnmSubmitDone tail, liverpool.cpp ~:659) | 3.48 ms (5.6 events/frame) | 0.21 ms (3.0 events/frame) |

- The GPU work itself is unchanged (DLSS evaluate 8.0 vs 8.2 ms) and the clocks do not explain the loss (they are higher with the sleep). All of the extra GPU time per frame is idle time between submissions, mostly at the end-of-batch flush of sceGnmSubmitDone.
- Mean sleep 8 ms. Reflex timeouts 0.

### Early submit (change)

While Reflex sleeps, the GPU command thread now submits the recorded work of a sceGnmSubmitDone batch every N draws and dispatches instead of only at the batch end (liverpool.cpp, EarlySubmit). Toggle bit ReflexEarlySubmit 134217728 in Disabled2 (bit set = old behaviour). Env GOW3_REFLEX_EARLY_DRAWS, default 80, 0 = never. Active only while Reflex sleeps (gow3_reflex SleepActive()).

| Session | Early submit | No early submit | Sleep off |
|---|---|---|---|
| Threshold 80, FPS | 54.8, 54.9 | 52.5, 52.5 | 65.7, 67.5 |
| Threshold 80, GPU ms/frame | 18.2 | 19.1 | - |
| Threshold 80, GPU idle ms/frame | 3.9 | about 4.5 | - |
| Threshold 24, FPS | 53.4, 53.2 | 50.9, 50.6 | 60.3 (one window had a 246 ms hitch) |
| Threshold 24, GPU ms/frame | 18.8 | 19.6 | - |
| Threshold 24, GPU idle ms/frame | 4.2 | 4.5 | - |

- Gain: about +2.3 to +2.6 FPS (about 4-5%), independent of the threshold (80 or 24). Reflex stays well below sleep off.
- The 3.4-3.5 ms gap at the SubmitDone flush stack did not move: it stays 3.41-3.48 ms with early submit on.

INFERENCE, not proven: with the sleep the GPU is drained at each batch, so the frame time follows the CPU chain (guest simulation and command production, stage A) instead of overlapping the previous frame's GPU work. The loss is then largely inherent to less pipelining, not a port stall that early submission removes. This fits the unchanged flush-stack gap above; no test isolated it.

### Frame Generation model (NGX)

NGX SDK 310.9.16 (tag v310.9.16) has no documented application-side parameter to choose the DLSS-G model or preset. The model (Preset B) can only be set through the NVIDIA App, so no launcher or menu selector was added. Header evidence: nvsdk_ngx_defs_dlssg.h has no preset or model parameter; the creation parameters include UserInterfaceRecompositionEnabled and LiteCandidateBlendingEnabled, which are not model selectors. Whether the NVIDIA App override reaches this NGX-direct port is unknown.

Also: a build made during packaging (gow3-probe.exe 12:56) exited silently right after boot twice; a rebuild with build.sh fixed it. Cause unknown, not investigated.

Not verified:
- Input latency with early submit was not re-measured.
- 1080p was not re-run.
- Frame Generation together with Reflex at 4K is untested.
- No confidence intervals: 2 windows per state, one session each.
- No independent concurrency review of EarlySubmit.
- Reflex logs (driver timings) were not collected in these sessions.
- Cause of the silent boot exit of the packaged build is unknown.

Toggle bit: ReflexEarlySubmit 134217728. Env: GOW3_REFLEX_EARLY_DRAWS.

## RivaTuner / MSI Afterburner OSD flicker: the present queue (22 Oct 2026)

Report (player, watching): the MSI Afterburner OSD flickered with Frame Generation on, and a "DLSS SDK -- DO NOT DISTRIBUTE" banner flickered too; with FG off the OSD still flickered.

Method. Desktop captures (C:\dev\bench\capscreen.ps1, GDI CopyFromScreen of the 2560x1440 screen; the port's own play.py shots are taken before the swapchain and never show third-party overlays), 120 per state, U3 bar (bench save), player's settings (1080p output, DLSS M, borderless, vsync, limit 120, Reflex on), RTX 5070, RTSS 7.3.7 (RTSS.exe still reports 7.3.5.28314 in its version resource; the uninstall entry says 7.3.7, build 28314 is the 7.3.7 release), MSI Afterburner 4.6.7 Beta 2. Classifier C:\dev\bench\osdclass.py: OSD present = share of strong horizontal edges (text with its shadow) in rows 30-100, columns 0-480 above 0.3% (frames without it ~0, with it ~4). Its first version counted white pixels and missed thin anti-aliased text: its 31%/52% readings were wrong and are superseded by the table below.

Banner: from the development NGX DLL, test setup only.
- gow3_pc/out/nvngx_dlssg.dll was the DLSS SDK 310.9.16 `dev` build (SHA-256 B49508DE..., same as C:\dev\DLSS\lib\Windows_x86_64\dev), copied by hand during the 22 Oct FG work; no script copies it. The player's install has the `rel` build (8D59D49B...). The banner was in 24 of 60 captures with FG on, never in the port's internal (real) frame, gone with FG off: NGX draws it into generated frames. Replaced by the `rel` build in gow3_pc/out (dev copy kept as C:\dev\bench\nvngx_dlssg.dev.dll): 0 of 120 captures since.

OSD (measured, 120 captures each):

| Present queue | State | OSD shown |
|---|---|---|
| Third queue (20 Oct default) | FG off | 0% |
| Third queue | FG on | 5% (8% with DISABLE_CAPFRAMEX_VULKAN_LAYER=1) |
| Third queue | FG off, limit 60 | 100% |
| Graphics queue (GOW3_PRESENT_QUEUE=0) | FG off / FG on / Reflex off / limit 60 | 100% each |
| Automatic (new build, graphics queue here) | FG off / FG on | 100% each; player: "parou de piscar" |

- RTSS is loaded in the game (RTSSHooks64.dll, RTSSVkLayer64.dll; CapFrameX's cfx_osd_vklayer.dll too). Disabling CapFrameX's layer changed nothing.
- RTSSHooks64.dll names (strings, not disassembly): hooks vkCreateDevice, vkCreateSwapchainKHR, vkQueuePresentKHR; uses vkGetDeviceQueue, vkQueueSubmit, vkQueueWaitIdle, fences; no vkCreateSemaphore. Inference, not proven by disassembly: RTSS draws the OSD inside the present hook with a submission on a queue it fetches itself, ordered by queue order rather than a semaphore, so a present on another queue can leave before the draw. That fits all rows: on the graphics queue the draw and the present share one queue; on the third queue at 120 FPS the present wins, at 60 FPS the draw has time.

Change: Instance::CreateDevice picks the third queue only when another adapter drives the screen (or that cannot be told): gpu/shim/gow3_display_adapter.cpp compares the DXGI adapter that owns the window's monitor with the Vulkan device LUID. Log line "GPU: presents on the graphics queue (the screen is on this GPU)". GOW3_PRESENT_QUEUE=1 forces the third queue, =0 the graphics queue. The 20 Oct third queue was made for hybrid laptops (cross-adapter presents under the submit lock froze the game 0.3-0.8 s); the desktop check then showed no gain from it.

Performance A/B (bench.sh, 4K output, DLSS M, limit 120, FG and Reflex on, MT on, one game session per window since the queue is fixed at device creation; C:\dev\bench\pqab.sh), real FPS by the port's count:
- Automatic (graphics queue): 54.6, 54.4, 53.9.
- Third queue: 54.2 in the one normal session; the A/A of the same day (third queue, 12 windows in one session) 54.6 [interval of the A/A difference -0.16, +0.30].
- No regression seen; too few sessions to claim a gain or loss in either direction.
- Third-queue anomalies, unexplained: one session never reached gameplay ("The game is not running"), one ran at 26.6 real / 53.2 shown FPS (p50 37.8 ms) in normal gameplay (end shot: the bar). Logs not kept (boot.log is overwritten per session). Not seen in the automatic sessions (3) nor in the A/A session.

Not verified: a hybrid laptop with this build (the automatic choice should keep the third queue there: the iGPU owns the monitor's DXGI output); other overlays (Steam, Discord, OBS); U1 and U2; exclusive fullscreen; HDR; the cause of the two third-queue anomalies; RTSS's draw path by disassembly.

## NVIDIA Streamline backend (23 Oct 2026)

Goal: DLSS super resolution, DLSS Frame Generation, Reflex and PC Latency through NVIDIA Streamline (SL) 2.14.16 instead of the port's NGX bridge, as an option. Setting `nv_backend=ngx|streamline` (GOW3_NV_BACKEND), read at start; default `ngx`.

Design:
- SL is loaded by the runtime itself (gpu/shim/gow3_streamline.cpp), not by the MSVC bridge: the headers are MIT (vendored in gpu/third_party/streamline, `sl_nvperf.h` left out, Nsight Perf licence), the DLLs are loaded at run time. Only that file includes them, compiled with -std=gnu++20 (under C++23 sl_pcl.h aliases std::to_underlying and does not compile).
- Load order: `sl.interposer.dll` from the exe folder, NVIDIA signature checked (sl::security::verifyEmbeddedSignature; GOW3_SL_UNSIGNED=1 accepts development DLLs), slInit with eDisableCLStateTracking | eUseManualHooking | eUseFrameBasedResourceTagging (the last one is needed by slSetTagForFrame), OTA off, projectId of the DLSS bridge. Any failure prints "Streamline: not used (<reason>); the NGX bridge stays" and the NGX path runs as before. The missing VC++ 2015-2022 runtime is one such reason (LoadLibrary error).
- Vulkan routing (vk_platform.cpp, vk_instance.cpp): only vkCreateInstance, vkEnumeratePhysicalDevices, vkCreateDevice, vkCreate/DestroyWin32SurfaceKHR, swapchain create/destroy/get images, acquire, present and vkDeviceWaitIdle go through SL's proxies. Everything else stays on vulkan-1: SL's vkGetDeviceProcAddr also wraps hot vkCmd* calls (its log: "Hook sl.common:Vulkan:CmdBindPipeline is NOT supported"), which the recorder threads do not need.
- Super resolution (vk_dlss.cpp, Dlss::ViaStreamline): viewport 1 with its own frame counter. The SL mode comes from slDLSSGetOptimalSettings (PickDlssMode, gpu/shim/gow3_streamline_mode.h): the smallest optimal size that holds the port's input, with the mode's minimum at or below it. Unit test `streamline-mode-test`.
- Frame generation (vk_presenter.cpp): viewport 0, frame index = Reflex flip serial; inputs tagged per frame with slSetTagForFrame; the backbuffer tag only when the image is letterboxed (with a full-size tag SL warned "Invalid backbuffer resource extent"). DLSS-G is unloaded at device creation when frame_gen=0 and loaded/unloaded later with a swapchain recreate (Swapchain::RecreateAround; Swapchain::Destroy now clears the handle, the recreate otherwise destroyed it twice).
- Reflex (gow3_reflex.cpp): markers, sleep and reports through SL's Reflex and PCL. With SL frame generation on, Reflex is forced to at least On: DLSS-G reports eFailReflexNotDetectedAtRuntime without it.
- Launcher (Display) and in-game menu ("Applied on restart") offer the choice when sl.interposer.dll is present.
- Packaging: STREAMLINE_SDK=<SL release> adds sl.interposer/common/dlss/dlss_g/reflex/pcl.dll (release license.txt: MIT) and licenses/nvidia-streamline. The nvngx_* DLLs of the release (RTX SDK licence) and NvLowLatencyVk.dll (Reflex SDK licence) stay out; the script refuses NvLowLatencyVk.dll. The player still adds nvngx_dlss.dll / nvngx_dlssg.dll.

Frame generation only interpolates with the window focused:
- Automated sessions (game started in the background) gave SL frame generation 60 real and 60 shown FPS in most runs, 120 shown in some. SL's verbose log (GOW3_SL_LOG=2): "dlfgPresent.cpp [shouldInterpolate] DLSS-G disabled: window not focused", then "interpolation state changed from disabled to enabled" once the game window got the focus. NGX frame generation does not depend on the focus.
- C:\dev\bench\focusgame.ps1 gives the game window the focus after gameplay is reached (FOCUS=1 in bench.sh, slsmoke.sh, slab.sh). Focused, U3 meat market, 1080p, model K, FG on, Reflex off: 60.0 real / 120.2 shown FPS, shown p50 8.33 ms, p99 9.57 ms, 1% low 99 FPS (NGX in slab2, same case: 60 / 120.1, p50 6.07, p99 12.19 ms, 1% low ~80). One session; not an A/B.
- Limiter fix: before, Presenter::FrameGenActive was true whenever the mode was on, so the frame limit halved real frames while SL showed none generated (60 real / 60 shown at a 120 cap). Now the presenter reads slDLSSGGetState every frame on the present thread and counts shown frames only while SL presented 2 frames for the last real one (one frame late). Unfocused check: about 86 real FPS, no generated frames, no halving. After 2 s on but not interpolating the log says so once, with whether the window has the focus. The FrameGen (Streamline) stats line now has with_inputs= and doubled=.

Performance, interleaved A/B `slab2` (C:\dev\bench\slab.sh, slstats.py; one game session per window, 20 s standing, U3 meat market save; base = 2bf79ce build, ngx and sl = the new build; 3 rounds, rotated order; RTX 5070), real FPS by the port's count, medians, [bootstrap interval of the difference]:

| Case | base | ngx | sl | sl - ngx |
|---|---|---|---|---|
| sr1080k (1080p, DLSS K, no FG, no Reflex) | 88.73 (3) | 89.21 (3) | 88.56 (3) | -0.65 [-2.77, +1.52] |
| sr4k (player's settings, 4K DLAA, no FG, no Reflex) | 56.76 (3) | 55.49 (2) | 55.33 (3) | -0.16 [-0.50, +0.19] |

- sr4k: SL's 1% low was 1.22 FPS lower than NGX [-1.83, -0.70]. The base's first session (59.0 FPS) is high against all the others (55.1-55.8); without it the base sits with the rest.
- Upscaling through SL: no frame rate difference resolved.
- 4K, FG + Reflex on (fgrx4k): SL 43.15 real FPS (3 sessions) against base 47.53 (2) and ngx 47.36 (1), -4.4 FPS [-4.89, -4.05] against base. Those SL sessions were not focused (no generated frames), so the loss is not frame generation work; SL's Reflex sleep grew to about 12-13 ms per frame. An earlier focused SL run at 4K: about 41.5 real / 83 shown, NGX 47.5 / 95. Not explained.
- slab2's FG sessions with SL are invalid for frame generation (not focused). The focused FG A/B (slab3) was stopped by the user after one session (base fgrx4k: 48.7 real / 97.5 shown).
- Harness: slab2 lost sessions when switching executables ("no gameplay", boot.log locked); slab.sh now copies the variant's exe with retries before each session.

Environment note: since about 09:20 on 23 Oct every build, the 2bf79ce one included, shows SyncInterval 0 and AllowsTearing in PresentMon (Hardware: Independent Flip). Not caused by this change; cause not looked for.

Seen once, not investigated: in one SL 4K screenshot Nate's head was missing in an idle animation (C:\dev\bench\sl_fg4k_on2.png); other SL shots show it. One NGX 1080p FG session (slab2 fg1080k_ngx_3) ran at 76.9 real / 104.3 shown instead of 60 / 120.

Not verified: the regression checks (RTSS / Afterburner OSD with SL, Vulkan validation, U1 and U2 boots, a soak), the focused FG A/B (user stopped it), Reflex latency through SL (PCL markers in PresentMon or NVIDIA's overlay), the 4K Reflex loss's cause, packaging with STREAMLINE_SDK (script not run), a PC without the VC++ runtime (fallback path read, not run), HDR, exclusive fullscreen, AMD/Intel GPUs (SL should not load features there; not run).

### Streamline as the only option (23 Oct 2026, later)

The player asked for Streamline as the only option, with NGX kept as a fallback.
- The `nv_backend` setting is gone: the menu row, the launcher choice, the ini key and the restart check. An old `nv_backend=` line in an ini is ignored. `GOW3_NV_BACKEND=ngx` (or 0) keeps Streamline off for A/B tests.
- Streamline is not loaded without `nvngx_dlss.dll` (it would only run Reflex, which the direct path covers) or without an NVIDIA DXGI adapter (AMD/Intel PCs: its vkCreateDevice adds its plugins' extensions and features). If the chosen Vulkan device is not NVIDIA's (a hybrid laptop's other GPU picked in the settings), Instance::CreateDevice calls Gow3Streamline::Abandon (slShutdown) and puts back vulkan-1's vkEnumeratePhysicalDevices, vkCreateDevice and surface entries before the device is created.
- In-game menu: when DLSS runs on the NGX bridge, the "NVIDIA Streamline" row gives the reason Streamline did not load. The Frame Generation help (menu and launcher) says that frames are only added while the window has the focus.
- Packaging without STREAMLINE_SDK now prints a warning.

Tested: build.sh. One U3 session (1080p, DLSS K, FG and Reflex off) with no GOW3_NV_BACKEND: "Streamline: SDK 2.14.16 loaded", "DLSS: through Streamline", "Reflex: through Streamline", 93 real FPS. PresentMon showed SyncInterval 1 again, without tearing, so the morning's tearing note no longer applies.

Not tested: the Abandon path (no non-NVIDIA GPU here), a PC without NVIDIA or without nvngx_dlss.dll, GOW3_NV_BACKEND=ngx after this change.

## Cartagena market: stutter and lower frame rate from readback faults (23 Oct 2026)

Report (player): walking back from the U3 chapter 2 save (the alley in front of the meat market's gate) through the market into the street, a stutter, then the frame rate settles lower. All of the day's earlier measurements stood still at the gate.

Method: C:\dev\bench\mktquick.sh / mkab.sh / mkabhere.sh (bench save, route analysis/play/routes/mkt_back.txt = 15 s walking back, frame log GOW3_FRAME_LOG with perf_counter marks, mktana.py / mqstats.py), player's settings (4K DLAA M, FG + Reflex on, cap 120), RTX 5070. A/Bs alternate the toggle file in one session, 10 s windows, order flipped each round.

Cause: guest threads' read faults on pages that pending readbacks read-protect (GOW3_ASYNC_READBACK=1, mode 1).
- Street (current build): ~250-290 per 5 s, ~900-1000 ms per 5 s across the game's threads. Stall frames (40-57 ms) held 30-55 ms of fault-handler time. Same route with mode 2 (pages left readable; unsafe, 16 Oct Cartagena crash): walk + street 50.9 real FPS, p99 26.6 ms, no frame over 33 ms, against 40.6 FPS, p99 34.5 ms, 18 frames over 33 ms.
- ~70% were false sharing: the guest reading its own bytes beside GPU bytes on the same page (guest 0xe3c753 in PrepareInstanceForRendering, fg-draw-mgr.cpp, ~130 per 5 s; 0xea640e). Each went through the GPU command thread (~1.3 ms), which then unwatched the page.
- The rest read GPU bytes: all from one memcpy (guest 0xe3c6e7, found by unwinding the C runtime's memcpy in the fault statistics). PrepareInstanceForRendering copies the instance's bone matrices ([inst+0x110], numJoints x 48 bytes, written by the cloth solvers cs 1c957b01 / 809864d3) into a per-frame allocation, the skinning palette the GPU reads. 74-108 per 5 s, readback queued ~5.5 ms before the read, then ~8.5-9.5 ms waiting for the GPU (the GPU runs ~14 ms behind the command thread). The command thread waited in CompleteReadbacks meanwhile.
- New diagnostics (GOW3_FAULT_STATS=1): "read faults by kind", library callers unwound to the guest, "Guest reads of GPU bytes on their way back", "Compute rings (5 s)" (~2,600 compute-ring submissions a second, waiting 2.3-3.3 ms on average for the command thread), "Forwarded guest copies".

Changes (toggle file, second word; bit set = old behaviour):
- GuestFalseSharing (1<<28): a read fault on a page whose watching readbacks' GPU bytes the read misses makes the page readable on the faulting thread (PageManager::OpenForReads, under the page lock, only while those readbacks are the page's only read watchers) and queues the usual unwatch for the command thread. BufferCache keeps an index of the watching readbacks' pieces per page (IndexWatched / UnindexWatched, its own mutex) for these lookups. 182 such faults cost 1.2 ms per 5 s.
- GuestReadPrewait (1<<29): a read of GPU bytes waits for the readback's submission on the faulting thread first; the command thread then lands it without waiting. Command thread time on these faults 800-900 -> ~60 ms per 5 s; no frame rate change by itself (A/B in the street, two spots: 44.7/44.9 and 41.9/42.1 FPS on/off): the game thread still waited.
- ForwardedCopies (1<<30): a guest memcpy of 256 bytes or more whose source overlaps GPU bytes of a watching readback is made by the GPU (Rasterizer::ForwardGuestCopy: on the command thread, ObtainBuffer + vkCmdCopyBuffer, in stream order after the solver), and the destination comes back with the submission's small-write readback. ~110 copies per 5 s (~0.8 MB), 0.03-0.06 ms each; read faults on GPU bytes: 0.

Measured (all three bits, one session each, 3 rounds):
- Street after the walk (~1,300 draws): 48.0 against 43.8 real FPS (+9.6%), each round higher (+3.5, +3.4, +5.8); p99 28.8-30.7 against 32.4-34.6 ms.
- Crowded street (camera turned, ~2,500 draws): 51.4 against 45.0 (+14%), each round higher (+7.4, +5.9, +5.9); p99 27.9-30.6 against 30.2-32.9 ms; frames over 33 ms 3 against 6.
- Screenshots: characters and their cloth look as before. 1080p (FG and Reflex off) after the change: 75-77 FPS in the crowded street, the GPU command thread fully busy (idle 0.3%): the next limit.

The numbers above are from the initial forwarding prototype, not the subsequently hardened build. Source review found two correctness gaps: forwarding initially returned before the destination was read-protected, so an immediate CPU read could see old RAM; and a new readback could add a watcher to an early-opened page without restoring its protection (the watcher count was already nonzero). ForwardGuestCopy now queues an exact protected destination readback, submits it and establishes its watches before returning. PageManager tracks early-opened pages under the page lock and restores protection when a new read watch arrives. Forwarding refuses GPU-side callers, overlapping ranges, unaligned Vulkan copies and overflowing/oversized ranges.

Further CPU work (not yet measured): RingAssociative (Disabled2 bit 31) uses 16,384 four-way sets for constant-ring snapshots instead of 4,096 direct-mapped slots. It retains the existing epoch, CPU/watch-token, GPU-write and ring-lifetime checks; no live guest pointers substitute for snapshots. ComputeSpecScratch (bit 32) reuses compute-specialization resource-vector storage, rebuilding every descriptor and runtime field on every dispatch. Profile st1: CollectRingBindings 31.6% inclusive, CopySparseMemory 13%, compute GetProgram 10.9% (specialization construction 6.1%).

Rejected experiment: queued fault/cache commands on stage B instead of draining it (temporary bit 31, removed before RingAssociative). Street cs1 81.1/74.2 FPS on/off was invalid: draws varied from 488 to 2,816. Static gate cs2 94.2/95.8, no consistent gain. The code was removed, not left enabled.

Not verified: performance of the hardened forwarding build; U1 and U2 (memcpy forwarding applies to any game, only when a source is under a readback watch); a long session; laptops. User requested fewer tests; no regression suite or stability soak run.

Short hardened check `ringassoc1` (1080p gate, FG/Reflex off): 25.8 FPS with ring/spec optimizations off, 31.2 on; repeated "Stuck fault" readback landings at 0x103c561000, versus the earlier 94–96 FPS gate baseline. The user subsequently reported high external PC load during the tests: this comparison does NOT establish a regression or a useful ring-cache gain. Forwarding remains opt-in (`GOW3_FORWARD_COPIES=1`), preserving destination-read protection. The old 9–14% unsafe-prototype gain must not be claimed for the default build. `ringassoc2`, forwarding disabled: 82.1/88.2 FPS off/on in one six-to-ten-second comparison, external load unmeasured; preliminary only.

Retest requested after the external-load report: C:\dev\bench\retest_cpu.py, one session, six-second windows, original-path baseline bracketing the changes. Records GetSystemTimes and per-process CPU deltas (external cores, total CPU and top external processes) for each window. No processes terminated except its own game. Gate windows compare constant-ring/spec changes and the opt-in stage-B command experiment; the backward market route then compares guest-read handling and protected forwarding. The stage-B path is restored as opt-in (`GOW3_COMMANDS_STAGE_B=1`, Disabled2 bit 34), with pending unknown task writes preventing stage-A snapshots ahead of the command. No repeated rounds or regression suite requested.

`cpu_retest1` results (1080p DLAA K, FG/Reflex off, six-second windows; `C:\dev\bench\cpu_retest1.load.json` and `.boot.log`):

| Location / enabled change | Real FPS | p99 ms | External CPU cores |
|---|---:|---:|---:|
| Gate, original path before | 88.4 | 15.5 | 2.05 |
| Gate, snapshot cache + specialization scratch/direct variant checks | 97.3 | 15.9 | 1.47 |
| Gate, stage-B commands only | 86.0 | 15.9 | 2.01 |
| Gate, original path after | 81.3 | 18.6 | 2.45 |
| Street, original path before | 77.0 | 17.3 | 1.73 |
| Street, guest-read handling | 79.2 | 16.3 | 1.51 |
| Street, forwarding enabled | 82.7 | 14.6 | 1.08 |
| Street, original path after | 82.4 | 15.0 | 0.99 |

Interpretation: the baseline itself moved materially with external load. Cache work is promising, not an isolated percentage gain. Stage-B commands have no demonstrated gain at comparable measured external load (86.0 vs 88.4). No "Forwarded guest copies" line occurred and GPU-byte fault counters stayed zero in this CPU-bound session: the forwarding window did not exercise forwarding and cannot validate its performance. A second short session at the player's 4K DLAA M + FG/Reflex settings is needed for that path; no extra CPU rounds. No stuck-fault/assert/device-loss lines in cpu_retest1. Toggle file restored to 0 0 and the test's game stopped.

The direct compute-variant check (Disabled2 bit 33, ComputeVariantCheck) was added before this retest. Buffer-only compute variants validate all current descriptors, binding validity, stride/format/swizzle fields and RuntimeInfo against the previously used specialization; on any mismatch they run full construction. No resource addresses or contents are assumed immutable. It shares DescribeBuffer with the full constructor to avoid divergent field rules.

`cpu_retest4k` (4K DLAA M, FG/Reflex on, backward route to the street, four six-second windows): original before 39.7 FPS (p99 32.8 ms, 1.17 external CPU cores); guest-read handling 43.6 (35.3 ms, 2.13 cores); protected forwarding 42.9 (32.7 ms, 1.55 cores); original after 44.1 (33.7 ms, 2.52 cores). Forwarding was exercised: 118 GPU copies / 5 s (~848 KB); GPU-byte read fault time fell to 151.7 ms in one reporting interval. However, this is NOT a valid FPS comparison: Streamline lost window focus during the guest-read/forwarding sequence (log: "game window does not have the focus", doubled fell from all frames to zero), and external CPU load also changed. Do not use these numbers as a gain/loss claim. The previous 25–31 FPS result was not reproduced, but its cause is not resolved. Eleven "Stuck fault" notices appeared across the whole run, including before the forwarding window; not attributable to forwarding alone. No assert/exception/device-loss report found. Game stopped and original toggle mask 0 0 restored. No further sessions started in that retest task: user requested fewer tests.

## Structural crowd snapshots and output grouping (23 Oct 2026)

The user authorized invasive structural experiments, provided the exact previous state remains
recoverable. Local checkpoint **c48f91f** contains the preceding CPU experiments and engineering
notes; its matching unstripped executable is `C:/dev/bench/cpu_before_structural_20261023.exe`.
Nothing pushed and the installed playable package has not been replaced. Do not reset unrelated
work to return: leave the new environment overrides unset (their paths are opt-in), or build the
checkpoint separately / use the preserved executable for an exact executable-level comparison.

**Persistent immutable mesh snapshots.** Only read-only slots 0..4 of crowd compute `8f067540`,
1 KiB to STREAM_THRESHOLD, qualify. A bounded 64 MiB append-only mapped buffer never overwrites an
allocation; CPU write/unmap watch versions invalidate lookups, not already queued GPU bytes.
Each watch promotion gets a unique increasing version; a range checks every page and takes the
maximum version, so dropping/re-watching ANY page changes its answer. The global watch generation
avoids walking unchanged ranges. Exact address/size checks reject compact-key collisions.
GPU-written, backing-written and pending-written inputs are refused, changing bone slot 5 stays
on the original ring path, and capacity exhaustion falls back without reclaiming in-flight bytes.
No generic asynchronous memcpy, live guest pointer or omitted shader work is used.

`C:/dev/bench/staticmesh_ab.py`: one U3 session per version, backward market route ending on the
street, 1080p DLAA K, FG/Reflex off, four eight-second OFF/ON/ON/OFF windows, external CPU load
recorded. These are short CPU-isolation measurements, NOT the player's 4K settings and NOT
independent replications or an A/A-calibrated gain:

| Test / window | Real FPS (S events) | p99 ms | External CPU cores |
|---|---:|---:|---:|
| staticmesh1 off / on / on / off | 79.8 / 79.4 / 81.6 / 79.8 | 16.3 / 16.4 / 16.0 / 15.9 | 1.41 / 1.81 / 1.52 / 1.44 |
| staticmesh2 off / on / on / off | 78.2 / 79.9 / 78.9 / 78.5 | 15.9 / 15.8 / 16.2 / 15.6 | 2.16 / 2.30 / 2.44 / 2.10 |

staticmesh1 qualifies/reuses the pool but retains mapping/dirty checks per buffer; staticmesh2
tries residency BEFORE those checks and adds sampled byte verification. Its `.boot.log` records
985 uploads, ~4.5 MiB resident, about two million hits per full five-second ON interval, no
capacity failures, **29,870 sampled byte comparisons and zero mismatches**. The `.png` records
the actual street view. Neither test establishes a material overall FPS or p99 improvement.
Keep this prototype opt-in (`GOW3_STATIC_MESH_SNAPSHOTS=1`, Disabled2 bit 35 restores transient
snapshots live). `GOW3_STATIC_MESH_CHECK=1` samples every 256 uses, independently of the expensive
full transient-ring check; a mismatch refuses the snapshot and takes the original path.

**Per-dispatch output grouping.** The existing `prof_st1` stage-B sampling report 7 (matching
executable in that folder; symbolized with `symprof_k.py`) attributes 23.5% inclusive to BindBuffers,
15.4% to ObtainBuffer, 4.7% to GpuRangesAdd and 10.7% to CompleteReadbacks. This is an older profile,
not a new measured speedup. The next experiment acquires/synchronizes slots 6..8's overlapping
+0/+12/+20 output views once per dispatch, while preserving every descriptor offset/size and
exact per-view hazard range. Only the observed contiguous equal-size layout qualifies; no holes
become GPU-written. Null, differently sized, unmapped, oversized or small-write
history-threshold-crossing groups take the original path. No memo survives the dispatch.
Opt-in `GOW3_SKINNING_WRITE_BATCH=1`; Disabled2 bit 36 restores separate acquisitions live.
This is NOT dispatch fusion and does not reduce the shader dispatch count.

`skinwrite1` did NOT exercise grouping: an unnecessarily strict formatted-store exclusion kept
the output views on the original path. Source inspection of BufferCache::SynchronizeMemory
shows its is_texel_buffer branch runs only for READS; written storage descriptors have the same
acquisition semantics whether formatted or raw. The next version removes that exclusion.
Do not claim the test's 1.9 -> 76.2 FPS transition as an improvement: its OFF window hit an
existing severe stall before grouping could run. Log evidence: repeated 0.5-1.0 s frames,
Wait profile `buffer_cache.cpp:2667` (CompleteReadbacks -> scheduler.Wait) up to 997.5 ms,
low game CPU (0.57 cores), and a stuck read of page 0x103c561000. The cause of these long waits
is NOT established by the log. Later ON/OFF windows, which both used separate acquisitions,
were 79.8/81.1 FPS at 1.21/1.19 external cores. Retain this run as stutter evidence, not a
performance comparison. Its `.on.png`/`.png` show the intended street scene.

`skinwrite2` exercised grouping (1080p DLAA K, FG/Reflex off, static snapshots disabled):
OFF/ON/ON/OFF 74.5/78.0/77.8/74.0 real FPS; p99 18.7/16.5/16.2/18.0 ms; external CPU
2.22/1.97/1.99/2.40 cores. Full ON reporting intervals record ~594-606k groups and ~1.2M
redundant acquisitions saved per five seconds, zero refusals. This verifies the path runs, NOT
an isolated 5% gain (external load was lower ON). `skinning-writes-test` passed with asserts
enabled, checking exact union coverage, offsets, unequal sizes, overflow, gaps and the 4 KiB
history boundary. No assert/device-loss/stuck-fault line in that run; screenshots are limited
visual evidence, not proof that every vertex/readback is correct. Other games/long sessions not
tested. The feature stays opt-in.

### Updated diagnosis at 4K, before further changes

`C:/dev/bench/market_bottleneck1` is one short U3 backward-market diagnostic (4K DLAA M, Reflex
on, FG off, 120 cap, MT on; BOTH new structural paths off). Contains matching executable,
CPU stacks every five seconds at a 3 ms sampling interval, GPU-label timestamps, submit timeline,
exact stationary window (`window.json`), start/end screenshots and boot log. Instrumented run:
absolute FPS and costs are NOT a clean performance comparison. The GPU label "DLSS evaluate
(NGX)" is a legacy hardcoded profiler string; startup confirms the Streamline backend.

The saved street scene's last GPU windows show DLAA evaluation ~7.6-8.1 ms/frame, crowd compute
~1.2-1.4 ms/frame for ~1,530 dispatches, and ~5-6 ms/frame BETWEEN submissions. The last term
does not establish a CPU-saturation bottleneck: it includes CPU/guest dependencies, pacing and
Reflex. The 16 s stationary window used 3.45 logical CPU equivalents for the game, 1.46 external
(40.9% system CPU). Worker startup reports `6 (+1 scanner, 12 hardware threads)`; Available()
counts the process's allowed affinity mask, so helpers are not restricted to six CPUs.

Sample report 13 (1,394 command-thread / 1,399 stage-B samples): CollectRingBindings now 13.8%
inclusive of the command thread, GetComputePipeline 6.2%, DrainDrawPipe 13.7%, and ProcessLoop
kernel waiting ~23%; stage B's BindBuffers 14.3%, ObtainBuffer 9.1%, wait/backoff ~40.4%.
These percentages are per-thread wall samples and overlap through inclusive callers; NEVER
sum them into frame time. They do not support "add workers until all twelve CPUs are busy" as
the fix at 4K. The earlier 31.6% CPU-bound profile is not the current 4K bottleneck ranking.

The concrete guest dependency remains: one report records 107 reads of solver matrices at
guest RIP 0xd23c6ec, ~1,016 ms summed guest-thread wait per five seconds; the average GPU wait
for such a read was ~9.07 ms. A later interval has 69 reads / 689 ms. These are SUMS across
threads, not seconds added directly to frame time. The CPU then copies the matrices into the
skinning palette that the GPU reads. More idle CPU helpers cannot execute through that fence.
The half-to-one-second stall from skinwrite1 did not reproduce in this diagnostic; its cause
remains unverified.

### Deferred guarded forwarding prototype

To remove the per-copy submission/round trip without the earlier stale-destination window,
`GOW3_FORWARD_COPY_DEFER=1` (also needs `GOW3_FORWARD_COPIES=1`, Disabled2 bit 37 restores the
old submitted-copy path) queues the destination's readback and protects it BEFORE memcpy
returns, including when its tick has not been submitted. CPU faults on an unsubmitted tick go
to the command thread to submit it, never prewait for an unsignalled tick. Strict memcpy pages
are excluded from both false-sharing opens and queued unwatch requests. CPU-only upload pieces
use the backing RAM view in this mode, avoiding a copy-worker fault while submission waits for
that worker. GPU ownership exclusions remain in the upload selection; no GPU-owned upload is
replaced with stale CPU bytes.

`GOW3_FORWARD_COPY_VERIFY=1` samples one accepted copy per 128: captures known source bytes,
deliberately reads a destination-page neighbour then the destination immediately after the
forwarded operation, compares bytes and asserts on mismatch. Only this diagnostic sample lands
the source early; normal forwarded operations do not. Results below cover sampled runtime
verification, not a general correctness or FPS claim. Generic copy forwarding remains
opt-in and this is not a released market fix.

`copydefer1` subsequently built and ran the guarded protocol (4K DLAA M, Reflex on, FG off,
static pool/output grouping disabled). OFF here means the PREVIOUS protected GPU-forwarding
path, not a no-forwarding baseline; both modes have GOW3_FORWARD_COPIES=1. Four six-second
OFF/ON/ON/OFF windows: 39.8/39.7/40.0/42.2 real FPS, p99 30.2/31.2/31.1/34.3 ms, external
0.95/0.73/1.40/3.20 CPU equivalents. No throughput or p99 improvement demonstrated. Around
150-160 copies/five seconds ran; moving their submission did not materially accelerate the frame.
14 diagnostic immediate-read byte comparisons passed, zero mismatches; four were in the deferred
ON phase (10,320-byte copies). The other checks include a 1,584-byte case on the previous path.
This verifies those samples only, not all races, map/unmap, other games or long sessions.
Three stuck-fault notices occurred, including two before ON, no assert/exception/device loss
found. Screenshots show the expected street scene. Mask restored to 0 0, INI bytes restored and
the script's own game stopped. The final rebuild adds guard-allocation assertions and corrects
the queued-age statistic for a strict false-sharing read (one diagnostic line had a zero epoch
and printed an impossible age); no further game run for those diagnostic/assertion additions.

**Conclusion after this experiment:** readback waits are real and a concrete synchronization
cost, but eliminating/forwarding an individual wait is NOT proof it is the frame's critical
path. Earlier statements treating that cost alone as the sustained-FPS cause were too strong.
The updated 4K evidence shows the largest individual GPU pass is DLAA M, plus serialized
CPU/guest/GPU dependencies and pacing gaps; there is no single proven whole-market fix yet.
No new optimization is enabled by default and no released FPS gain is claimed.

Submit timeline cross-check for the same 16 s stationary diagnostic: all three schedulers'
GPU timestamp spans, aligned by the minimum host-done minus GPU-end offset and merged, cover
12,482 ms of 16,000 ms (~78%); ~3,518 ms lies outside those spans. This is NOT ALU utilization
and not evidence all gaps are avoidable. Instrumented S-event throughput 44.6 FPS, p99 32.1 ms.
File modification times mapped to the monotonic clock confirm CPU reports 13 and 14 fit wholly
inside the stationary window (not the backward walk). Raw evidence stays outside the repository.

Final checks: runtime rebuild succeeded; `skinning-writes-test` passed again (asserts enabled),
`git diff --check` clean apart from Git's CRLF-conversion notices; no game instance left running.
No regression suite/FG comparison/other-game tests, following the request for fewer short tests.
Structural source changes remain uncommitted; the earlier checkpoint remains c48f91f.

## Reflex pacer: market frame rate (24 Oct 2026)

Setup: U3 Cartagena market street after the mkt_back route, 4K DLAA model M, RTX 5070, cap 120.

Cause (measured, `C:/dev/bench/market_bottleneck1`): the frame was paced by the Reflex sleep,
not by GPU or CPU throughput. The driver/Streamline sleep averaged 12.7-14.6 ms per ~24 ms
frame. GPU busy 17.5 ms of 22.4 ms per frame; the GPU sat idle ~9 ms before the next frame's first
submission in ~43% of frames (262 gaps over 8 ms holding 2,687 ms of 16 s). Frame Generation
forces Reflex on, so FG players always paid this. Inferred: this is why the 23 Oct CPU
experiments (crowd dispatch fusion, fusion1: 43.8/43.3/43.3/42.0 FPS off/on/on/off, opt-in,
0d209c7) showed no gain at 4K: the sleep, not the CPU work, set the frame time.

Change (`gow3_pc/gpu/shim/gow3_reflex.cpp`): with Reflex effectively on, the guest thread no
longer calls the driver's/Streamline's sleep. `PacerWait` waits until the draw scheduler's work
timeline reaches frame (id-2)'s `ready_tick` (recorded lock-free by `FrameQueued` in
`Presenter::PrepareFrame`), 50 ms timeout. The GPU always has the next frame queued and the CPU
stays about one GPU frame ahead. Markers, Reflex mode and FG forcing are unchanged. `Disabled2`
bit 39 (`ReflexPacer`, 549755813888) restores the driver sleep live. `GOW3_REFLEX_PACER_LAG`
(1-4, default 2). The stats line adds `pacer=`, `pacer_waits=`, `pacer_avg_ms=`,
`pacer_timeouts=`. EarlySubmit (SleepActive) is off while the pacer runs.

Measurements (measured; `C:/dev/bench/reflex_ab.py`, one session each, 8 s windows in order
A,B,B,A,A,B, real FPS from S events; external CPU load 0.4-2.6 cores, per window in the .json):

| run | A | B | A FPS (p99 ms) | B FPS (p99 ms) |
|---|---|---|---|---|
| reflexfg1 (FG+Reflex) | driver sleep | no sleep at all | 42.1/42.3/42.2 (31.5-33.0) | 53.1/53.8/53.5 (24.5-25.0) |
| pacerfg1 (FG+Reflex) | driver sleep | pacer | 51.7*/43.4/41.5 (29.4-32.2) | 54.9/55.0/54.7 (21.5-23.9) |
| pacernofg1 (FG off, Reflex on, final build) | driver sleep | pacer | 59.9*/50.6/49.1 (25.5-29.0) | 62.3/63.8/64.2 (21.0-23.8) |

\* first window, driver sleep still ramping (pacerfg1: 3.7 -> 13 ms). FG doubled every real frame
in both arms. pacernofg1: `pacer_timeouts` 0, no assert/device-loss lines.

Latency (measured from the Reflex log, simStart->gpuRenderEnd, pacerfg1, FG on; the driver's
per-frame attribution under FG is not reliable): pacer p50 36.1 ms, p90 37.5; driver sleep
p50 20.5, p90 46.0. Trade-off: about +15 ms p50, much shorter tail, +27-30% real FPS.

Tried and removed: an adaptive extra delay after the wait (feedback from GPU-idle-at-submit).
The first signal never fired (pacerfg2: delay saturated at 16 ms, 42-45 FPS). The fixed signal
converged to 0-2.4 ms (pacerfg3: 53.7/54.1/53.9 FPS, latency p50 35.1 ms): no latency or FPS
gain. Inferred: the CPU chain fills the frame, so there is no slack to trade for latency.

Hitches, found, not fixed: the 0.5-1.5 s market hitches are the GPU holding one already-submitted
submission for multiples of ~500 ms (skinwrite1/mkd/fusion1 logs; "GPU wait: 1 seconds,
target=15768 completed=15767 submitted-next=15777"). The repeated "Stuck fault" on page
0x103c561000 is a consequence. Cause not established. Next diagnostic: `GOW3_GPU_CHECKPOINTS=1` +
`GOW3_SUBMIT_LOG` with a 200 ms WAIT_TIMEOUT in `vk_semaphore.cpp`, or an ETW/GPUView capture.

Not verified: U1/U2, other U3 chapters, long sessions, input latency by an external tool,
laptops/non-NVIDIA (the pacer only runs where Reflex is on, i.e. NVIDIA), the installed player
package (not updated).

## Market hitches: diagnostics, not reproduced (24 Oct 2026)

Diagnostics added (opt-in except one stats line): `GOW3_WAIT_TIMEOUT_MS=<50..10000>` shortens the GPU semaphore wait poll in `vk_semaphore.cpp` (default 1000 ms as before) and prints, on every timeout (max 50 lines), "GPU wait: N ms ..." plus the stuck submission: scheduler (draw/present/flip, names set by `GOW3_SUBMIT_LOG`), tick, age since submit, each wait semaphore with its value and current counter, and the last 16 `bindSparse` calls when it waited on the sparse semaphore. Each scheduler keeps a 512-entry ring of submission records (O(1) write under the existing submit_mutex). `BufferCache::SubmitPendingArenaBinds` prints "Sparse binds (5 s): N submits, B binds, U unbinds, X MiB" (only in 5 s windows with binds). Bench script: `C:/dev/bench/hitch_hunt.py <tag> [RES=WxH] [GOW3_KEY=VALUE ...]` (120 s of movement in the street after mkt_back, lists frames over 150 ms).

Hypothesis tested: a slow `bindSparse` holding the draw queue. Measured in hitch1: 3-208 bindSparse submits per 5 s, binds only, 0 unbinds.

Not reproduced (measured, S-event intervals over 200 ms after the first 20 s of a session):
- Today, build with the pacer: pacerfg1, pacerfg2, pacerfg3, pacernofg1, hitch1 (4K FG+Reflex, checkpoints on), hitch2 (4K Reflex/FG off, checkpoints on), hitch3 (1080p DLAA K, Reflex/FG off, checkpoints off, the skinwrite1 conditions): 0 in all seven; largest street frame 146 ms. hitch3's only long interval was 14 s of initial loading at 1.2 s, before gameplay.
- Earlier sessions (22-23 Oct builds): 6 of 28 had street hitches: skinwrite1 17 (0.5/1.0 s, Reflex off, 1080p), mkd 7, mky 2, mkab1 1, fusion1 1, ringassoc1 1.
- 0 of 7 against an earlier rate of about 6 in 28 is not significant (chance about 0.2-0.3); the hitch is NOT shown fixed. In skinwrite1 the first stall (453 ms, "GPU ticks 439.6 ms", 0 compiles, 0 faults) precedes any read fault: the GPU stopped finishing an already-submitted submission; the stuck faults on page 0x103c561000 follow from it.

Next time it happens: run with `GOW3_WAIT_TIMEOUT_MS=150 GOW3_SUBMIT_LOG=<file>` (optionally `GOW3_GPU_CHECKPOINTS=1`) and read the "GPU wait: stuck" lines.

## U3 Sully shirt stretched: LDS out of bounds (24 Oct 2026)

Report: U3 ch. 2 Cartagena, young Sully's back shirt tail hung as a long strip to knee height
(player video 24 Oct). Long-standing: also in the 20 Oct build (ab_ring_base.exe).

Ruled out (user visual check each): 30 FPS/FG off; Disabled2 bits 28-33; GOW3_STALE_READBACK=0 +
GOW3_DEFERRED_READBACK=0 + MT off; GOW3_LDS_BARRIERS_MULTIWAVE=1 and + GOW3_LDS_BARRIER_REGIONS=1.
GOW3_CLOTH_PEEK: solvers 1c957b01/809864d3 0 exploded, 0 outliers.

Cause: cs 1c957b01 runs 1024-thread groups with 2 KiB LDS declared; it zeroes lds[tid] up to 1023
and reads lds[tid+512]. GCN drops LDS writes past the allocation and reads 0; the port's SPIR-V
shared array had the declared size (+ wave64 scratch) and no bounds check, so NVIDIA corrupted the
reductions. Evidence: a temporary build with an 8 KiB array brought 1c957b01 output maxima from
slot0 305 -> 2.7, slot3 49-86 -> 1.8, slot17 5-16 -> 0.2, slot23 55-68 -> 0.07, and the user saw a
normal shirt. GOW3_WAVE64_ALL_BALLOTS=1 made 1c957b01 explode every dispatch (waist hole),
confirming the tail comes from it.

Fix: shader_recompiler/ir/passes/shared_memory_bounds_pass.cpp (SharedMemoryBoundsPass, before
LowerWave64BallotPass): compute-shader game LDS loads past the declared size read 0,
writes/atomics go to an 8-byte dummy slot behind it (atomics return 0); GDS and in-range
immediates untouched; wave64 scratch starts after the dummy. GOW3_LDS_BOUNDS: default all compute
shaders, 0 off, or hex list. SPIR-V cache names get "_ldsb" (all shaders recompile once).

Verified: user saw a normal shirt with the fix at the player's settings (1440p, 165 cap, FG on);
runtime build ok; street ~57-59 FPS in that run, no assert/device-loss lines.

Limits (from an adversarial review): the pass is skipped when the 8-byte dummy would push the shader
past the device's shared memory limit (a shader declaring exactly maxComputeSharedMemorySize keeps
the old unchecked accesses). LDS that SharedMemoryToStoragePass moves to a storage buffer (runs
earlier) gets no bounds handling, as before. Without explicit workgroup layout, u64 LDS ops split
into u32 halves are checked per half.

Not verified: U1/U2, other U3 scenes, performance A/B of the extra compare/select per LDS access,
AMD GPUs.

## 24 October 2026: launcher overhaul

The launcher was rebuilt around a sidebar (launcher/uncharted_launcher.pyw; the palette and the
drawing/widget helpers moved to launcher/widgets.py). The CHANGELOG's Unreleased entries dated
24 Oct 2026 carry the player-facing wording; this is the technical record.

What changed:

- Layout: the games and the settings sections sit in a sidebar on the left; the launcher opens the
  section used last. Sections renamed (SECTIONS/OLD_SECTIONS): Quick Setup -> Home (page_setup),
  Cheats -> Gameplay (page_cheats), Advanced -> System (page_advanced). The window fits a 1280x720
  screen (1080p at 150% scaling); pages scroll on a short screen and PLAY stays at the bottom
  right. Long hints wrap at the window's width; a choice list with more than 9 options (the frame
  rate limit) is a menu instead of a segmented control.
- Home: a "Ready to play" checklist above the presets (readiness(), kept a plain function for
  tests) checks the game files, update 1.02 and whether its files are intact
  (install_update.py check_update), the NVIDIA DLSS / Frame Generation DLLs, the drive's speed
  (slow_drive_reason) and a new launcher release; each line that needs attention carries the
  buttons that fix it. The NVIDIA file buttons moved here from Display; adding one no longer needs
  a restart (rebuild_sections() rebuilds the Display and Performance pages in place, since their
  option lists depend on the DLLs). The slow checks (update archive, drive) run one at a time in
  the background (refresh_home) and are re-asked if the state changed meanwhile.
- ? hints: every setting row has a one-line `short` hint; the full text sits behind a round ?
  (InfoTip, hover/click/Space). Settings shared by the three games (language, speakers, the
  unlocks, the cheats, the performance log, update checks, idle game threads) carry an ALL GAMES
  badge; per-game settings stay in the game's own ini.
- Greyed options: the dependent Display choices (DEPENDENT_OPTIONS = preset/dlss_model/frame_gen)
  are disabled with an orange note line under them (refresh_dependencies) while their condition is
  missing; nothing is reset, values survive and the rows wake up when a matching anti-aliasing is
  chosen, in the launcher or in the in-game menu.
- Apply to all games: a button at the top right of Display and Performance copies the selected
  game's graphics and performance settings to the other two (controls untouched); it asks first
  and is disabled while a game runs. Changing the resolution, aspect ratio, multithreading or
  graphics card while a game runs now notes it applies at the next game start.
- Themed dialogs: Dialog/Toast (widgets.py) replace the native message boxes - dark windows in the
  launcher's colours, Enter answers the highlighted button, Esc cancels, long details get a Copy
  button, short confirmations are corner toasts. The game-crash dialog offers Open log folder and
  Copy report (the error, the game and its version, the GPU, the main settings); it also covers
  the desktop game shortcuts (`--play`).
- Keyboard: Tab/Shift+Tab with an orange focus ring, Space/Enter press, Left/Right change choices,
  Down/Enter open a list; shortcuts Ctrl+1/2/3 (game), Ctrl+Tab / Ctrl+Shift+Tab (section),
  Ctrl+Enter (play or stop). In Controls, Enter on a slot starts the key capture and Delete clears
  the binding.
- Controller: the SDL gamepad (Gamepads, already used for pad bindings) drives the whole launcher -
  D-pad/left stick move, A presses, left/right change choices, B goes back, LB/RB switch the
  section, Start plays or stops; in dialogs A presses the highlighted button. It only polls while
  the launcher window has the focus and no game runs.

Fixed along the way:

- The mouse wheel stopped scrolling the pages after rebinding a key: the key capture bound the
  wheel/keys sequences with bind_all and unbound them afterwards, which took the page scrolling
  with it until the launcher restarted. <MouseWheel>, <KeyPress> and <ButtonPress> are bound once
  for the whole session now (test_wheel_binding_survives_a_capture).
- Reset (System -> TROUBLESHOOTING -> "Reset this game's settings") wrote the setting defaults but
  left the controls as they were: reload_ini (at start, game switch) restores key_map/mouse_map/
  pad_map from the game's ini, so the old bindings came back. Reset calls save_controls() now, as
  Reset controls under Controls always did (test_reset_puts_the_default_controls_in_the_ini).
- The launcher froze for seconds on blocking calls: the NVIDIA DLL signature check (up to 60 s of
  PowerShell), shortcut creation, stopping the game (taskkill) and the Vulkan device list (a child
  process, up to 10 s) moved into background work off the Tk thread; the Graphics card row joins
  Display when the device list arrives.

Contracts kept:

- The per-game settings file (probe.ini, shared with the in-game menu) and all its keys are
  unchanged; probe_uncharted.py turns them into the same GOW3_*/UNCHARTED_* variables.
- launcher.json stays version 8 (no migration runs; version 7/8 already turned multithreading on
  once and a later "off" stays off). The stored last-tab key is mapped through OLD_SECTIONS
  (setup->home, cheats->gameplay, advanced->system; unknown -> home, test_old_tab_names_map_to_
  sections), so an in-place update opens the section the player used last.
- `--play u1|u2|u3` (desktop game shortcuts, GitHub #25) still starts the game directly with the
  saved settings and no launcher window; the crash dialog also covers those starts.

Tested: gow3_pc/tests/test_uncharted_launcher_ui.py (Tk, emulated input: readiness cases, every
section builds, old tab mapping, apply-to-all, wheel during capture, reset, stop in a thread,
dialog buttons, hint/badge coverage, greyed options, keyboard, controller steps).

Not verified: real controller input (a controller in hand; the gamepad nav was checked with
synthetic events only); real Tab traversal and the focus ring on screen (emulated); Ctrl+Tab on
Windows with real key events (Tk's Control-Tab binding quirk; emulated only); 150% DPI on a real
screen (the scale was emulated); Wine/Proton; a real game-crash dialog from a game that actually
crashed (driven with a fake error); the Open log folder button; the FSR 4 / DLSS greyed rows seen
on screen; the packaged zip carrying launcher/widgets.py (package_windows.sh copies it, the zip
itself not built); an in-place launcher update from an older version (the updater path was not run
after the change); the option search that was in the plan was dropped and not built.

Merged with upstream 0.6.0 (origin/main 6870e36, 24 Oct 2026): its launcher changes were carried
into the sidebar layout. The performance counter's rows (Show performance counter, What it shows
with the item pills, corner, layout, size, background) are in Display; the pill widget (`Chip`)
moved to launcher/widgets.py and takes the keyboard focus like Toggle. Save profile is in System
with a short hint; vibration, the 4x/5x sensitivities and the Tk 9 side-button numbers came in
unchanged. The crash note for a 1.00 game without the update is added to the reason before the
themed crash dialog (show_crash) and names System -> GAME UPDATE. Not verified: these rows on
screen (only the launcher UI tests ran).

## U3 chapter 2 frame drop: player capture, stage B limit, two opt-ins re-measured (24 Oct 2026)

Report (player): the frame rate falls and stays low in one stretch of U3 chapter 2 (Cartagena: alley, museum entrance, `col-call-to-friends-*`, `col-pipe-climb-a`). Player's settings: 1920x1080 output, DLSS preset 1 model K, Frame Generation and Reflex on (pacer), VSync on, cap 165; installed build 0.5.0 = main 4a489cf.

Capture (measured; player's install, launcher diagnostics plus `GOW3_FRAME_LOG` through test-env.txt, 34 windows of 5 s, ~170 s from the chapter start):
- Light windows: 80-106 real FPS at 500-1,700 draws/frame.
- Heavy windows: 48.7-62 real FPS at 1,900-3,300 draws/frame, about 6,300-6,600 pipe items/frame (draws plus compute dispatches).
- In every heavy window: stage B busy 84-90%, stage A waited 18-28%, GPU command thread idle 0.2%, pipe 300-380k items/s. FPS follows pipe items/s divided by items/frame. 0 compiles, no memory pressure (video memory 4.6 of 11.2 GB). Write faults on game threads ~380 ms per 5 s and DMA sync 111 ms per 5 s on the GPU thread: small next to the stage limit.
- Verdict: CPU-bound on the serial draw pipe stages, the known limit ("Remaining CPU limit (profile)", 20 Oct), not the GPU.

Profile (measured; C:/dev/bench/prof_cap1, mkprof.sh, Cartagena street after mkt_back, 1080p DLAA K, FG/Reflex off, GOW3_PROFILE=10 at 5 ms; stage B busy 88%): stage B window 7, 1,810 samples, inclusive: RunDrawPacket 83%, BindResources 45.4%, DispatchRecord 36.5%, DrawRecord 34.5%, BindBuffers 21.5%, ObtainBuffer 13.3%, CompleteReadbacks 10.2% (LandGuarded 7.6%), BindTextures 6.4%, GpuRangesAdd 4.4%, SynchronizeMemory 3.6%. Flat: no single hot spot.

Crowd dispatch fusion (GOW3_DISPATCH_FUSION=1, player's own session, measured): no gain (54-64 real FPS in the same stretch, pipe items/s unchanged at ~360k). Cause (code reading): nearly every batch is a singleton (151 "singleton" lines = ~7M singles, ~80k/s; boundary Scheduler::SignalAfterHostCopies, vk_scheduler.cpp:209, because every Record() flushes the open deferred batch). Even when it fuses, AppendSkinningBatch runs after BindResources, so all per-dispatch binding work stays on stage B: it cannot shorten stage B. Left opt-in.

Parallelizing stage B (design review, code reading only, estimates not measured): almost all of stage B's per-item work is ordered (ObtainBuffer, SynchronizeMemory copies, barrier tracker, readback landing and its on_landed guest fences, texture cache). A prepare thread for the pure part (V# fetch, garbage classification, ClampRangeSize, MakeUserData) removes an estimated 3-8% of stage B, 2-5% net after the handoff. A split by domain (cache bookkeeping vs recording) reopens the 18 Oct readback ownership race. Stage A is ~97% busy and waits on stage B ~20% of its time, so any stage B gain tops out near +20-25% FPS. Not done. Cheapest exact probe if this is picked up: stage A passes its compute V#s and clamp/garbage results in the dispatch packet (it already computes them in PostDraw), guarded by the guest memory table generation, behind a new Disabled2 bit with a verify mode; estimated 1.5-3.5% of stage B.

Skinning write grouping (GOW3_SKINNING_WRITE_BATCH=1, Disabled2 bit 36), measured: C:/dev/bench/opt_ab.py (new: reflex_ab.py at the player's probe-u3.ini and 1080p, live toggle of one Disabled2 bit, 8 windows of 8 s ON/OFF/OFF/ON/ON/OFF/OFF/ON), Cartagena street after mkt_back, FG and Reflex on. The path ran: 640-750k groups and 1.28-1.50M acquisitions saved per 5 s, 0 refused. Result: on 81.3 vs off 80.9 real FPS (+0.4); per window 78.6-84.6, rising through the session; p99 15.3 vs 15.2 ms; external load 1.35-2.57 cores. Inside the noise: not turned on by default.

Not verified: the player's exact worst spot (the bench street is lighter: ~80 FPS against his 48-55); U1 and U2; a second replicate of the A/B. Note: two other Claude sessions ran U3 benches on the same analysis/uncharted-user folder during this work (14:09-14:35); a first A/B attempt collided with them and was discarded. The profile at ~13:50 may have shared the CPU with them; its stage B 88% busy matches the player's capture.

## Crowd skinning dispatch runs (24 Oct 2026, opt-in)

Player report: in U3 chapter 2 Cartagena, crossing a line from the meat market gate into the crowded street drops the frame rate; the user wants the street to run like the rest. Tests at 4K output, DLSS Quality, Frame Generation + Reflex (pacer) on, RTX 5070, Ryzen 5 5600, bench save backup_ch2meat (gate) and route mkt_back (street).

What the street adds (measured; C:/dev/bench/line1.*, line2.* with GOW3_DISPATCH_STATS=1, old build):
- Gate 86 real FPS at 700 draws/frame, street 78 at 1,290 draws/frame. Both CPU-bound on the draw pipe (~400k pipe items/s; stage B 78-86% busy).
- Both sides: crowd skinning cs 8f067540 = 1,522 dispatches/frame, 1x1x1, one consecutive run (max run 1,524), stage A 2.4 us + stage B 2.9 us each: ~3.7 ms of stage A and ~4.4 ms of stage B per 12-13 ms frame. The street only adds draws (stage B draw time 1.8 -> 3.3 ms/frame).

Change (GOW3_DISPATCH_RUNS=1, Disabled2 bit 40 DispatchRuns, bit set = old path, read live; vk_rasterizer.cpp/.h, liverpool.cpp cut hooks, vk_compute_pipeline.cpp/.h, vk_dispatch_batch_spirv.cpp, vk_skinning_writes.h):
- Stage A stages consecutive 8f067540 instances (same pipeline, queue, dims except dim_x grouping, unchanged registers, all read slots in the constant ring or static mesh snapshots, written views valid) into one PacketKind::DispatchRun packet of up to 64, with up to 4 RELEASE_MEM per instance staged into the run (their labels pending until the packet is committed). Any other packet, queued command, drain, yield, submission retire, end of queue or a refused instance cuts the run first, so stream order is unchanged.
- Stage B records one vkCmdDispatch(dim_x, count, 1) with the batch SPIR-V (workgroup Y = instance; per-instance descriptor arrays, capacity 64, own descriptor pools recycled by tick, layout support checked). Written views are joined into exact spans (no gap becomes GPU-written); spans over 64 KiB are acquired as chunks in (32 KiB, 64 KiB] so no 1 MiB window tick is stamped (views over 64 KiB and views of 4 KiB or less are acquired one by one, as before); texture invalidation stays per view; write side effects run once the batch is certain; more than 32 spans, a moved tick that does not settle, metadata writes or anything unproven replays the instances through the unchanged DispatchRecord. pending_writes is pruned after every run flush.
- GOW3_DISPATCH_RUN_VERIFY=1: stage B replays every run (frame on the old path) and compares the batch plan with the old bindings. It checks stage B's binding plan only, not stage A's staging or the bookkeeping side effects.
- "Dispatch runs (5 s)" line with packets, instances, replays and cuts by reason, chunked spans, window-tick changes, re-acquisitions, pending_writes size and prunes.

Measured:
- Verify (runv5/runv6): ~500-560k dispatches compared per 5 s; descriptor, push data, tracked range and written byte mismatches 0 in every line; window-tick changes 0; no ASSERT/device lost/VK_ERROR.
- Runs only (runv8): ~32.8k run packets of 18.6 dispatches per 5 s, 610k dispatches batched; replays 0 (over 32 spans 0, tick moved 0); window-tick changes 0. Pipe items ~396k -> ~182k/s.
- Live A/B, C:/dev/bench/opt_ab.py with OPT_RES=3840x2160 (player's probe-u3.ini at 4K, 8 windows of 8 s alternating the bit, street after mkt_back): first build 80.7 vs 76.9 real FPS (runab4k); after the review fixes 84.4 vs 78.6 real FPS, +7.4%, every on window above every off window, p99 15.7 vs 16.1 ms (runab4k2).
- With runs on in the street, PresentMon: GPU busy 9.9 of 12.3 ms (79%), still CPU-bound; GpuCommandProcessor (stage A) 91%, DrawRec 90%, VkRecorder 83%. Stage A is now the limit (profile C:/dev/bench/prof_runs: CollectRingBindings 26%, DrainDrawPipe 19.7%, LandDeferredFenceFor 11.7%, RefreshComputeKey 10.9%).
- GOW3_STATIC_MESH_SNAPSHOTS=1 on top of runs (bit 35 A/B, meshab4k): 84.4 vs 83.1 real FPS, small but every on window at or above every off window.

Reviews: two Opus reviews and a workflow review (two reviewers) found no corruption/hang/crash; their exactness items (per-view texture invalidation, window ticks on large spans, pending_writes prune starvation, side-effect order, pool lifetime, layout limits, hull over 32 spans, tick moving before Record) are fixed.

Not verified: U1 and U2 (their treasure effects also run 8f067540, with garbage V#s that runs refuse); other U3 chapters; long sessions; the re-acquisition and over-32-spans branches never ran in a session (counters only); 1080p. The feature stays opt-in until those are checked.

## Upstream 0.6.0 merged (24 Oct 2026)

Merge of upstream v0.6.0 (7b04bac) into claude/merge-upstream-0.6.0 (base a77e0c6 = 0.5.0, ours 92e7b85). Four files conflicted; the other 27 upstream files merged on their own. Decisions (ours = fork kept, theirs = upstream taken):
- tools/probe_uncharted.py, the GOW3_PIPELINE_BINARIES block: both. Ours keeps GOW3_DISPATCH_RUNS=1 and GOW3_STATIC_MESH_SNAPSHOTS=1; theirs sets GOW3_PIPELINE_BINARIES=0 (both sides had 1; upstream measured U2 15.5 GB RAM with them vs 7.0 GB without, 40 s vs 9.6 s pipeline load) and deletes the old pipeline-binaries folders on a background thread. Auto-merged in the same file: RUN_GROUP_PATCH (U2 0xd09665), GOW3_USER_ID (save profiles), GOW3_MIP_TARGETS=1 for U3, UNCHARTED_COOP_OVERLAY. The fork's U1 multithreading text (U1 follows the launcher) stays; upstream did not change that block.
- Duplicate patch check: RUN_GROUP_PATCH's signatures (`4c8b27 488b4c24f8 4a8d1c31 ...`, particle run grouping, 1.02 U1 0x5fa2f0 / U2 0xd09580 / U3 0xf6bef0) match no fork table; the fork's tables added since 0.5.0 (1 ms job-worker wait in JOB_SLEEP_PATCHES, movie pacing hook in probe.c) are at other sites. No overlap, nothing dropped. The patch is a multi-change entry, so the fork's skip-on-mismatch in apply_patches does not change it.
- tools/apply_update.py: both. Theirs (installed-files.txt manifest, carry_over moves every player file back, update-kept-* rescue folder) is the more complete fix; ours (CARRY copies nvngx_dlss.dll / nvngx_dlssg.dll inside the install step) is kept because it still holds when carry_over has to put all of gow3_pc aside: the DLLs are then already in the new gow3_pc\out, and carry_over skips them as present.
- launcher/uncharted_launcher.pyw: INI_DEFAULTS both (ours: preset, dlss_model, gpu_id, anisotropy, reflex, frame_gen; theirs: overlay_* keys). GAME_SETTING_DEFAULTS: ours multithreaded=True plus their save_profile="1". Display page: our anisotropy and graphics card rows plus their "Show performance counter" toggle and overlay_panel; add_ngx (ours) and OVERLAY_ITEMS / overlay_panel (theirs) both kept as methods. Dropped upstream's second `import tkinter.font as tkfont` (the fork already imports it).
- CHANGELOG.md: the fork's "Unreleased (fork, after 0.5.0)" section stays on top; upstream's 0.6.0 notes go into it as "From BilalReloaded's 0.6.0 (merged 24 Oct)", above the 0.4.1/0.5.0 subsection (as at the 0.5.0 merge). tools/github/CHANGELOG.md: theirs (identical to v0.6.0).
- tools/VERSION: theirs, 0.6.0. The updater reads the fork's releases (REPOSITORY espindoIa/...), and the fork now contains 0.6.0; at 0.5.0 a fork package would call any 0.6.x fork release an update for the wrong reason and misstate what it holds.
- shader_recompiler/resource.h (no textual conflict, one edit): upstream's new "base 0 or size 0 reads as null" rule in IsGarbageEffectBuffer returned before the fork's live gate, so Disabled2 bit 1024 (SpecGarbageSharps) no longer turned the whole filter off. It now returns `!garbage_sharp_filter_on || garbage_sharp_filter_on()`. Effect on dispatch runs: such V#s were already `unbound` for ClassifyGarbageSharp, so the run planner refuses them as before (ReplaySharps); they now also make fewer 8f067540 variants.
- runtime_coop.c: built by the existing globs (build.sh `src/runtime*.c`, CMakeLists GOW3_RUNTIME); probe.c calls runtime_coop_install, off unless GOW3_COOP_CMD / GOW3_COOP_BOOT.

Toggle bits: upstream 0.6.0 adds none (no Disabled/Disabled2 use in its diff); the fork's Disabled2 bits up to 1ull<<41 are unchanged. Fork pieces checked present after the merge: DispatchRun* / StageDispatchRun (vk_rasterizer.cpp), liverpool.cpp cut hooks and RunComputeReleaseMem (liverpool.cpp not touched by upstream), static mesh snapshots, gow3_reflex.cpp, Streamline backend, LDS bounds pass (none touched by upstream), launcher Reflex / FG / Streamline options.

Not verified: any game run (boots are the user's); the save profile, vibration and overlay rows in the launcher GUI; direct mouse aim with the fork's aim patches; GOW3_MIP_TARGETS with the fork's scene-resolution and upscaler paths in U3; dispatch-run counts with upstream's null-V# rule; carry_over on a real update.

## Game FXAA skipped under temporal AA (24 Oct 2026)

Evidence: U2 capture C:\dev\cap\frame_20261017-214933_3.txt, passes #54-#56. #54 (ps 484cdbf5 plus
HUD draws) writes the frame into the display buffer (R8G8B8A8Srgb). #55 (ps 6bd1e015) copies it
into a temp RGBA8Unorm with luma in alpha. #56 (ps 749bbec9) is FXAA 3.11 quality 39 (subpix
0.75, edge thresholds 0.166/0.0833; decompiled from the shader dumps) and writes back into the
display buffer. Same hash in the U1/U2/U3 caches. The port's upscaler runs earlier, at cs
aec539f0 on the HDR scene, so FXAA softened an already anti-aliased image and the HUD.

Design: tools/probe_uncharted.py sets GOW3_GAME_AA_PS=749bbec9 in the temporal-AA branch.
TemporalUpscaler::SkipsGameAa(ps_hash) is true when it matches, Active(), !composite_frames and
Disabled2(GameFxaa) is clear. The rasterizer's draw returns right after the OnComposite block when
the draw's colour target is a display buffer and SkipsGameAa is true (bind_prepared cleared as in
the OnComposite return); the frame-start gate is unchanged. #54's image stays in the display
buffer; only alpha differs (0 instead of luma). The presenter ignores display alpha:
vk_presenter.cpp:799 (view alpha swizzle eOne) and :810 (same on the upscaled path), swapchain
composite alpha opaque (vk_swapchain.cpp:359); post_process.frag passes alpha through but sees 1.
Toggle: Disabled2 bit GameFxaa = 1 << 46 (70368744177664; 1 << 40 and 1 << 41 are DispatchRuns and RunReleaseBatch). Bit 1 << 31 (2147483648) is already
RingAssociative, so it could not be used. Set = the game's FXAA runs.

Not verified: in-game A/B (pending), U1/U2/menu.

Measured (24 Oct 2026, 14:14-14:16):

- U3 bar after Continue, 2560x1440 DLAA, model K, frame dumps (GOW3_FRAME_DUMP_TRIGGER, display
  buffer), Laplacian variance x1e4 of luma; wall crop 1100-1353 x 373-547 (the 21 Oct 4K crop
  scaled to 1440p).
- One valid pair (same pose): FXAA skipped wall 2.27 / frame 10.13; FXAA on wall 1.51 / frame
  8.35 (+50% wall, +21% frame; run-to-run noise ~9% from 21 Oct).
- Rounds 2-3 are not usable: Nate turned between dumps, then the game paused and the window was
  closed by the player mid-run.
- Not verified: more pairs, the player's DLSS Quality 3840x2160 setup, U1, U2, the menu, motion.

## U3 blur suspects: jitter and DOF diagnostics (24 Oct 2026)

Suspects: (A) scene draws left without the upscaler jitter, (B) the depth-of-field factor target.
In DrawRecord the jitter goes to draws with the scene depth only when num_indices > 6 or more than
one instance; smaller non-instanced draws (decals, quads) get none, so under DLSS they would sit
about 1 px off the jittered scene. Three opt-in switches, all off by default (no behaviour change):

- `GOW3_JITTER_STATS=1`: counts the draws with the scene depth under an upscaler (the
  GOW3_JITTER_PS full-screen passes are left out). Every ~5 s: `Jitter stats: <frames> frames,
  jittered x/frame, unjittered y/frame; top unjittered ps: <hash> x<count> (idx n inst m) ...`
  (top 5, index and instance count of the last draw seen). Frames = Gow3Stats::gpu_frames delta.
- Disabled2 bit `JitterSmallDraws` = 1 << 47 (140737488355328), inverted sense: set = those small
  draws get the jitter too. Opt-in because a full-screen quad with the scene depth may leave an
  edge column unwritten. Set it in disabled-optimizations.txt (second number) and A/B with the
  stats line and a frame dump.
- `GOW3_TARGET_DUMP_PS=<hex>[,<hex>...]` + `GOW3_TARGET_DUMP_TRIGGER=<file>`: creating the file
  arms the next GPU frame; after each draw with a listed pixel shader (low 32 bits of the hash)
  its colour target 0 (whole image, mip 0 layer 0) goes to `$GOW3_DUMP_DIR` (default out/dump) as
  `target_<hash>_<frame>_<n>_<w>x<h>_<vkformat>.raw`, written when the GPU is done. At most 8 per
  trigger. The pass ends after such a draw (the next draw begins a new one), so only use it to
  measure.

Not verified: none of the three was run (build only).

Measured (24 Oct 2026, same run):

- GOW3_JITTER_STATS: ~336 jittered and 5.0 unjittered scene-depth draws per frame; all unjittered
  are 6-index single-instance draws of ps c11d3e2c (2/frame), 0bae6524, ab30daa9, f76d1599
  (1/frame each). Not A/B'd with JitterSmallDraws; whether they are full-screen quads is not
  checked.
- DOF factor (ps 4c610459 target, 2560x1440 R8): 0 in every pixel of the gameplay frame, so the
  tonemap's DOF blend is 0 there: suspect ruled out for this scene (aiming and cutscenes not
  checked).

## 25 October 2026: engine flight recorder

Why: one-frame flashes, flickers and ghosts are reported but cannot be predicted, so no capture is
running when one happens, and a key pressed afterwards (the session recorder's F9) only catches the
frames after it. The engine already knows the numbers that would explain most of them (an upscaler
reset or not, a camera cut, a draw dropped while its pipeline compiled, a texture evicted, a stall),
but only prints 5 s summaries. The flight recorder keeps those numbers for every presented flip in a
ring and writes the ring out after the fact. This also answers open item 8 of the 24 Oct audit list
above ("no camera-cut detection"): the recorder does not fix it, it shows whether cuts go unnoticed.

**Design** (`gow3_pc/gpu/shim/gow3_flight.{h,cpp}`).
- `Gow3Flight::OnFlip(movie, frame_gen)` is called at the top of `VideoOutDriver::Flip` (driver.cpp), which the present thread runs once per presented guest flip (early presents included, their later completion not). It runs whenever `GOW3_FLIGHT` is not `0`, independent of `GOW3_FRAME_STATS`.
- Hot path: an atomic-flag guard (a second caller drops its row, no mutex), one steady clock read, two `exchange`s (events and detail), ~17 relaxed loads of cumulative counters, 20 gauges, one 240-byte row written word by word into a preallocated ring slot under a per-slot sequence lock, the anomaly rules (a few float operations; a 32-value `nth_element` only when an interval is over 60 ms or the camera moved over the cut floor, 0.25 units; one `acos`). No lock, allocation, file access or print. Measured in flight-test (RelWithDebInfo, this machine, 25 Oct): `Record` 48 ns/call with a walking camera, 91 ns/call with the median run every flip, the whole `OnFlip` 65 ns/call. The first call allocates the ring, starts the dump thread and prints one `Flight:` line (once per process).
- Hooks elsewhere are one-liners: `Gow3Flight::Event(bit, detail)` (`fetch_or`), `Gow3Flight::Count(id)` (`fetch_add`), `Gow3Flight::Gauge(id, value)` (store of the float bits), `Gow3Flight::Camera(view_to_world, proj_x, proj_y)`; with `GOW3_FLIGHT=0` each is one load of a const flag. None changes rendering.
- Counters zeroed by the 5 s Frame stats printer (`g_gow3_compiles`, `g_gow3_pending_draws`, `tracker_faults`, `host_copy_waits`, `reduced_draws`, ...) are not read; the recorder counts compiles and skipped draws itself next to the same sites.
- Dump thread (one, started at the first flip, polls every 50 ms): takes the trigger file (`Gow3TriggerFiles::Watch("GOW3_FLIGHT_TRIGGER")`, the shared 250 ms watcher), holds up to 4 requests, and writes a request once 60 more flips were recorded or 2 s passed (so a request still gets written when the flips stop). It copies the rows out of the ring (rows being rewritten are dropped by the sequence lock; at most a few of the oldest are lost while it copies), builds the CSV, writes `<name>.tmp` and renames it. A failed dump prints one `Flight: dump <reason> failed: ...` line and goes on; a written one prints `Flight: wrote <file> (<n> flips)`.
- The trigger file is polled by the dump thread, not inside OnFlip as first planned: Take() locks a mutex when a file is pending, and a frozen game (no flips) could then never take an F10 "freeze" mark.

**Hook sites.**

| Site | Feeds |
| --- | --- |
| driver.cpp `VideoOutDriver::Flip` | OnFlip; MovieFrame (a movie frame decoded in the last 300 ms, `MovieActive`), fg_on (`Presenter::FrameGenActive`) |
| vk_pipeline_cache.cpp `CompileTimer` | Count Compiles, PipelineCompiled |
| vk_pipeline_cache.cpp `GetGraphicsPipeline` (both `++g_gow3_pending_draws`) | Count SkippedDraws, DrawSkippedPendingCompile |
| vk_temporal_upscaler.cpp `OnFrameStart` | UpscalerReset 1 (settings changed) / 2 (not run last frame), only when `reset` was clear; out_w, out_h |
| vk_temporal_upscaler.cpp `OnDispatch` | UpscalerReset 3 (trigger without scene color/camera), only when `reset` was clear |
| vk_temporal_upscaler.cpp `EnsureResources` | UpscalerFeatureCreated, UpscalerReset 4 |
| vk_temporal_upscaler.cpp `RunUiOnly` / `UpscaleToUi` (stretched) | StretchedOrUiFrame every such frame; UpscalerReset 5 / 6 when `reset` was clear |
| vk_temporal_upscaler.cpp `RecordFsr4` / `EnsureDlssFeature` / `RecordDlss` | UpscalerReset 7 (FSR 4 fallback), UpscalerFeatureCreated + UpscalerReset 8 (DLSS feature created), UpscalerReset 9 (DLSS fallback) |
| vk_rasterizer.cpp frame start (after `OnFrameStart`) | HistoryInvalidated (when OnFrameStart reports a change); jit_x, jit_y, lod_bias (`SceneMipBias`) |
| vk_camera_motion.cpp `OnConstants`, `OnFlatConstants`, `OnVelocityConstants` | cam_*, fwd_*, proj_* (velocity mode: proj = 2/c0, -2/c1 as `TaaDepthParameters`) |
| vk_camera_motion.cpp `OnDisplayPass` | NoCamera (velocity mode: no valid velocity camera this frame; it never sets `frame_has_camera`, so that flag would say "no camera" every Uncharted frame) |
| texture_cache.cpp GC pass (`++gc_evictions`) | Count GcEvictions, TextureGcEvict |
| gow3_flight.cpp OnFlip from `Gow3Settings` | scene_w/h (`active_render_width/height`), upscaler, preset, dlss_model |
| vk_presenter.cpp `PresentLoadingFrame` (after the image is acquired) | LoadingFrame |
| vk_presenter.cpp `Present` (own DLSS frame generation path) / `StreamlineFrameGen` | presents: 2 when `PresentGenerated` showed a generated frame, else 1; Streamline: `FgState::presented` (1 while its frame generation is unloaded or not wanted) |

**Contract** (the Python side is written against it; names are stable, documented in the header comment of `gow3_flight.h`).
- Environment: `GOW3_FLIGHT=0` disables everything (default on). `GOW3_FLIGHT_DIR=<dir>` output folder (default: `flight` next to `GOW3_DUMP_DIR`, i.e. `<parent of the dumps dir>\flight`, else `.\flight`). `GOW3_FLIGHT_TRIGGER=<file>`. `GOW3_FLIGHT_AUTO=0` disables anomaly dumps. `GOW3_FLIGHT_FRAMES=<n>` ring size (default 1024, clamped 64..8192). `GOW3_FLIGHT_CUT_MOVE=<units>` (default 0.25) and `GOW3_FLIGHT_CUT_DEG=<deg>` (default 30) tune the camjump rule; a value that is not a positive number keeps the default.
- File: `<dir>/flight_<YYYYmmdd-HHMMSS>-<mmm>_<reason>.csv` (local time), written as `<name>.tmp` then renamed. Manual reasons are the trigger file's first line in lower case with every character other than `[a-z0-9_-]` dropped, at most 24, default `manual` (the same rule as `flight_label()` in tools/session_recorder.py, which finds the dump by it); automatic ones `auto-stall`, `auto-reset`, `auto-skipped`, `auto-camjump`.
- Line 1: `# flight v1 unix_ms=<ms at dump> reason=<reason> frames=<rows> ring=<ring size> build=<exe file name>/<bytes>` (spaces in the exe name become `_`). Line 2: header. Then one row per flip, oldest first: the ring up to the request plus the next 60 flips.
- Columns (47): `seq, unix_ms, t_ms, dt_ms, marker, events, event_names, event_detail, d_draws, d_dispatches, d_gpu_frames, d_compiles, d_skipped_draws, d_gc_evictions, d_images_registered, d_image_upload_mb, d_buffer_upload_mb, d_copy_mb, d_submissions, d_gc_ms, d_frame_wait_ms, d_present_ms, arena_mb, image_mb, images_live, cam_x, cam_y, cam_z, fwd_x, fwd_y, fwd_z, proj_x, proj_y, jit_x, jit_y, out_w, out_h, lod_bias, scene_w, scene_h, upscaler, preset, dlss_model, fg_on, cam_jump_deg, cam_move, presents`. `unix_ms` is the start's system clock plus the steady time (no system clock read per flip). `marker` is 1 on the row nearest the request (the stall, the reset, the cut, or the flip when a manual trigger was taken). `events` is a `0x`-prefixed hex mask (Python `int(x, 0)` reads it), `event_names` the names joined by `|`: UpscalerReset, HistoryInvalidated, NoCamera, DrawSkippedPendingCompile, PipelineCompiled, UpscalerFeatureCreated, StretchedOrUiFrame, FrameGenOnOff, TextureGcEvict, MovieFrame, LoadingFrame (bits 0..10). `event_detail` is the last detail of the flip, the UpscalerReset reason 1..9 (table above; 10 = camera cut since the next section). `d_*` are changes since the previous row (0 on the first row); `d_submissions`, `d_gc_ms`, `d_frame_wait_ms`, `d_present_ms` come from timers that only run with `GOW3_FRAME_STATS=1` and are 0 otherwise. `cam_jump_deg` / `cam_move` are the forward-axis turn and the position change vs the previous row (0 when either row has no camera). `presents` is the frames shown for the last present before the flip (1; 2 with frame generation 2x; under Streamline its own count, which it reports one more frame late; 0 when Streamline gave no state).
- Automatic dumps (no `GOW3_FLIGHT_AUTO=0`): none in the first 5 s, 15 s cooldown per kind (skipped: 60 s), at most 30 per process, at most 4 pending requests in all. A "quiet frame" below is a row with MovieFrame, StretchedOrUiFrame or LoadingFrame (a row's events are those since the previous flip, so they cover the interval before it).
  - stall: an interval over max(60 ms, 4x the median of the last 32 intervals) (with at least 8 intervals seen), unless the row is a quiet frame or a quiet frame came within 2 s before the interval began (a level load, a movie's end).
  - reset: an UpscalerReset more than 5 s after start, with no quiet frame in the 2 s before; it is posted 2 s later, and dropped if such a frame comes in those 2 s (so the dump holds 2 s plus 60 flips after the marker).
  - skipped: a flip with DrawSkippedPendingCompile in gameplay (no quiet frame in it or the 2 s before). Every row still counts the skipped draws (`d_skipped_draws`, the event).
  - camjump: cam_jump_deg > 30 (`GOW3_FLIGHT_CUT_DEG`), or cam_move over 8x the median move of the last 32 flips and over a floor of 0.25 units (`GOW3_FLIGHT_CUT_MOVE`), with no UpscalerReset / HistoryInvalidated in the flip before, the flip itself or the 2 after (gauges are written on the GPU thread and events at the next frame start, so they can be a flip apart): the cut the upscaler did not notice, the flash suspect. The move rule is relative because the games' world units are not known; the floor only keeps a still camera's tiny moves from counting.
- Python side (tools/probe_uncharted.py and tools/session_recorder.py, written separately against this contract): when diagnostics are on, probe_uncharted.py sets `GOW3_FLIGHT_DIR=<out>\flight` and `GOW3_FLIGHT_TRIGGER=<out>\flight.trigger`. On an F7-F10 mark the session recorder writes the mark label into the trigger file, waits for a new `flight_*_<label>.csv` in that folder (up to `FLIGHT_WAIT_S`, 8 s in its first version; the task asked for 3 s) and moves it into the mark folder's `flight\`; at the end of the session it copies the session's `*_auto-*` dumps. Timing of a manual dump: trigger watcher up to 250 ms, dump thread up to 50 ms, then 60 flips (0.5 s at 120 FPS, 2 s at 30 FPS: the 2 s cap), then the write.

**Limits.**
- Flips only: frames made by DLSS frame generation get no row of their own (fg_on and `presents` say when they exist). Loading frames the presenter draws itself (`PresentLoadingFrame`) are not flips either; they set LoadingFrame on the next row. `PresentLoadingFrame` is only called during the start-up pipeline warm-up (`Presenter` constructor, with `GOW3_BACKGROUND_WARMUP=0`), before the first flip, so level loads are silenced by the UI-only and movie frames around them, not by LoadingFrame. A load with neither (game frames, then a long gap, then game frames) still gives an auto-stall dump.
- Numbers show causes, not pixels: a flash with no event and smooth gauges is not explained by this file.
- Gauges are the latest values when the flip is recorded; the GPU command thread may already be in the next frame.
- `UpscalerReset` 2 ("not run last frame") is reported once per streak, so a menu shows one reset, not one per frame.
- No loading-screen detector apart from UI-only frames (StretchedOrUiFrame); MovieFrame is the driver's 300 ms movie-decode test.
- No exposure gauge: the port passes no exposure value of its own (FSR 3/DLSS `preExposure` 1.0, FSR 4 auto exposure is an on/off setting), so there is nothing to record.

**Tests.** `flight-test` (`gow3_pc/tests/test_flight.cpp`, CMake target `EXCLUDE_FROM_ALL`, no Vulkan): ring wrap-around (64-slot ring, 200 flips), deltas and the 47 columns of every row, the comment line, the marker (and the nearest row when the requested one is gone), stall in and after the 5 s grace, the 4x-median rule at 50 ms flips (150 ms no, 250 ms yes), the 15 s cooldown, reset with the 2 s quiet time before and after, skipped with its 60 s cooldown and the 30-dump cap, quiet frames (a 3 s gap with a loading frame and a hitch 1 s later: no stall, a hitch 2.5 s later: stall; a hitch 1 s after a movie: no stall; a reset 1 s after a loading frame: no dump; skipped during a movie or 1 s after a UI frame: no dump, 3 s after: dump), camera cuts (90 degree turn, a reset next flip cancels, a 10 degree turn does not count, a 50-unit move does; at 0.01 units per flip a 0.2 move does not count and a 0.5 move does; at 0.5 per flip a 0.5 move does not; the two environment variables and bad values), the overhead (200,000 `Record` calls in two camera cases and 200,000 `OnFlip` calls, printed, under 2000 ns asserted), label sanitising (same results as the Python `flight_label`, path characters removed), the file name, the .tmp-then-rename write (replace, and a folder that cannot be made). `ninja -C out/gpu flight-test && ./out/gpu/flight-test.exe` exits 0; the whole port (`gow3_pc/build.sh`) builds.

**Not verified.** No game run (the player was playing): that OnFlip is reached at the expected rate, that a dump lands in `analysis\<game>\flight\`, the trigger round trip with the session recorder, the CSV of a real flash. The overhead is measured in flight-test only (synthetic rows, no game threads competing for the counters' cache lines), not in a game profile. Which gauges and events move at a real flash is unknown, and so is whether the camera gauges follow the shown camera in all three games (U1/U2 flat camera, velocity mode). The automatic rules' thresholds (60 ms / 4x median, 30 degrees, 8x median move over 0.25 units, 2 s quiet time, 60 s skipped cooldown) are untested on real sessions; whether Uncharted's level loads always show UI-only or movie frames (which now silence the stall rule) is not checked. `presents` was not seen with frame generation on.

**Game run, 25 Oct 2026 10:05-10:25 (U3 chapter 2 meat market save, Continue, 4K, player's ini, build a5e6d10).**
- OnFlip ran on every flip; `Flight: recording the last 1024 flips` printed once; with `GOW3_FLIGHT=0` no `Flight:` line at all.
- Manual dump: a trigger file with "flash" was taken within ~1 s and `flight_..._flash.csv` (1022 flips) written. (The first try failed only because the test command lost a backslash in GOW3_FLIGHT_TRIGGER: the game watched `...flightflight.trigger`; read from the process environment.)
- Automatic dumps at Continue (cold pipeline cache): auto-skipped, auto-stall (438 ms interval), auto-reset, then auto-camjump twice, then auto-stall every 40-60 s at 70-135 ms intervals.
- First finding: the frame where the scene appears after the menu (11 draws/frame before, 434 after) moves the camera 161 degrees and 106 units with 115 draws skipped for pending compiles, and no UpscalerReset or HistoryInvalidated in that flip or the next two: the temporal history crosses the cut (the static review's "no history reset after a load" finding, now seen in numbers; not yet fixed).
- `d_compiles` stayed 0 at the markers although PipelineCompiled events and skipped draws appeared: the Compiles counter hook in CompileTimer may not cover the background compiles (to check).
- FPS with the recorder on / off, 30 s each after a 15 s settle, one boot each, same save: on 54.8, off 54.1, on 54.5, off 59.5. Boot-to-boot noise (about 5 FPS, the last run had the warmest cache) is far larger than any recorder cost; no cost is visible, none is proven.

## 25 October 2026: history reset on camera cuts

Why: the flight recorder's first game run (section above, U3 chapter 2 meat market save, Continue,
4K) showed two unnoticed cuts per boot, the same in both boots: seq 184 (100 draws, 40 skipped for
pending compiles) turning the camera 168.2 degrees and moving it 102.3 units, and the frame where
the scene appears (434 draws, 115 skipped; seq 1848, and 1682 in the second boot) turning 161.5
degrees and moving 106.5 units, with no UpscalerReset or HistoryInvalidated in that flip or the
two after (auto-camjump dumps). The port had no camera-cut detection at all: the upscaler (DLSS, FSR 3/4, TAA) blended the
history from before the cut into the first frames of the scene (open item 8 of the 24 Oct audit
list).

**Rule** (`gow3_pc/gpu/shim/gow3_camera_cut.h`, header-only `Gow3CameraCut::Detector`). Once per
frame, on the frame's first camera (velocity pass constants c12..c23 in Uncharted's velocity mode,
the scene-constant camera of U1, the flat camera of U2), compared with the previous frame's:
- a cut when the forward axis turned more than 45 degrees, or
- the position moved more than max(floor, 16x the median per-frame move of the last 32 non-cut
  camera frames); floor 1.0 world unit, `GOW3_CUT_MOVE=<units>` changes it (a value that is not a
  positive number keeps 1.0), or
- the previous camera is older than 0.5 s (no camera frames in between: menus, movies, loads).
- The first camera ever and a camera without a usable forward axis (length outside 0.5..2) are
  not cuts; a cut's own move is kept out of the median.
- Conservative on purpose: mouse flicks reach ~30 degrees per frame at 60 FPS; walking at 0.05
  units a frame gives the 1.0 floor (16x would be 0.8), a vehicle at 0.5 a frame an 8-unit limit.
- The flight recorder's camjump rule uses the same defaults (45 degrees, 16x over a 1.0 floor;
  `GOW3_FLIGHT_CUT_DEG` / `GOW3_FLIGHT_CUT_MOVE` override) since 25 Oct, so a camjump dump means a
  cut the upscaler did not reset for. Its first thresholds (30 degrees, 8x, 0.25) were stricter.

**What a cut does** (`CameraMotion::CheckCut`, vk_camera_motion.cpp; `TemporalUpscaler::NoteCameraCut`,
vk_temporal_upscaler.cpp).
- CameraMotion sets `camera_cut` for the frame (cleared at the display pass). With the scene-constant
  and flat cameras it sets `previous = current` (identity reprojection: zero camera motion); in
  velocity mode `RecordVelocity` writes mode 3 (zero motion) that frame.
- `Run` and `RunScaled` call `NoteCameraCut()` before the motion pass and before every read of
  `reset` (TAA's reset flag, FSR 3/4 and DLSS `reset`, frame generation's `was_reset`): it sets
  `reset`, raises the flight events UpscalerReset with the new reason 10 `ResetCameraCut` (only when
  `reset` was clear, like the other sites) and HistoryInvalidated, and counts
  `Gow3Stats::camera_cuts`. With `GOW3_FRAME_STATS=1` the 5 s printer adds `Camera cuts: N (upscaler
  history reset)` when N > 0.
- The decision does not depend on `GOW3_FLIGHT`.

**Toggle.** Disabled2 bit 42 `CameraCutReset` (value 4398046511104, the next free bit after
`RunReleaseBatch` 1<<41), read live; bit set = old behaviour (no cut detection). The detector keeps
following the camera while the bit is set, so clearing it mid-run starts from the current pose.

**`d_compiles` stayed 0: cause and fix.** The flight CSV mapping was right (`row.cum[CCompiles] =
g_counters[Compiles]`, `delta(CCompiles)` in the right column). In the 25 Oct dumps PipelineCompiled
appears only in row 1 and never again while hundreds of draws were skipped: the background
compiles never reach `CompileTimer`. `CompileTimer` covers only the GPU thread's synchronous
compiles (`CompileModule`, the synchronous branches of `GetGraphicsPipeline` / `GetComputePipeline`).
The async shader jobs (`QueueShader`), the async graphics pipelines (`GetGraphicsPipeline`'s
`AsyncGraphicsCompilation`) and the compute precompiles (`RunPrecompile`) run on compiler workers
with only `PhaseTimer g_gow3_async_ns`. Fix (vk_pipeline_cache.cpp, `CountBackgroundCompile`): count
Compiles and raise PipelineCompiled where the GPU thread publishes each result: `PublishShaders`
(a shader or permutation registered), `PublishGraphics` (a pipeline emplaced), `Adopt` (a
precompiled compute pipeline added; not when the GPU thread compiled it meanwhile). One count per
completed compile; failed compiles are not counted. The 5 s `Frame stats` compile count
(`g_gow3_compiles`, GPU-thread compile time) is unchanged. No flight-test change: the cause was
outside gow3_flight.

**Tests.** `camera-cut-test` (`gow3_pc/tests/test_camera_cut.cpp`, CMake `EXCLUDE_FROM_ALL`, no
Vulkan): walking 0.05 units and 2 degrees a frame = no cut; a 35-degree flick = no cut; 161 degrees
with 106 units = cut; 50 degrees alone and 106 units alone = cut; the same camera 0.6 s later = cut,
0.4 s later = no cut; the 1.0 floor while walking; a zero forward axis ignored; a vehicle at 0.5 a
frame: limit 8, 0.9 no cut, 9 cut; `GOW3_CUT_MOVE` 4 / -3 / abc and a 4-unit floor. Exit codes:
`ninja -C out/gpu camera-cut-test flight-test` 0, `camera-cut-test.exe` 0, `flight-test.exe` 0
(all passed), `gow3_pc/build.sh` 0.

**Game check, 25 Oct 2026 10:36-10:40** (U3, walking with the left stick). Both Continue cuts
(168.2 degrees / 102.3 units at seq 188; 161.5 degrees / 106.5 units at the scene's first frame)
now carry UpscalerReset reason 10 + HistoryInvalidated. `d_compiles` now counts (87 and 149 in the
first two dumps). Walking and fast stick turns (up to 22.7 degrees and 1.72 units per flip) caused
no reset. One more reset came from the 0.5 s stale rule after an 812 ms hitch (turn 6.9 degrees,
move 0.43), which is intended (history older than 0.5 s). Auto-camjump fired once on walking (11
degrees, 0.73 units) with the old flight thresholds (30 degrees, 8x, 0.25 floor), while the
detector correctly did not; hence the flight defaults now match the detector (45 degrees, 16x,
1.0 floor). `flight-test` updated: small-unit worlds are tested with `GOW3_FLIGHT_CUT_MOVE=0.25`.

**Not verified (before that game check).** No game run: that the cut fires at U3's Continue frame (and with which rule:
stale, turn or move), that the ghost of the menu image is gone, how many cuts a normal session
shows (cutscene shot changes should count; a fast vehicle or a respawn might too), and that no
reset fires during ordinary play. U1's scene-constant and U2's flat cameras are not tested in a
game either; their `previous = current` path is code reading only. The world units (and so the
1.0 floor) are not known per game. With `RunScaled` called twice in a frame the cut would be
counted twice in `Camera cuts` (the reset itself is idempotent). `d_compiles` after the fix was
not seen in a CSV.

## 25 October 2026: scene LOD bias and anisotropy follow the shader's sampling

Why: the player reported lines/bleeding at texture edges and textures that look blurred or wrong.
Two port features change every scene sampler and had never been checked per texture:
- Forced anisotropy (`Rasterizer::ForceAnisotropy`, ca54d74, default 16x) raised every graphics
  sampler with a mip filter, without looking at the image. One-level textures (atlases, LUTs,
  decals, data textures) and samplers clamped to MaxLod 0 got 16x too; anisotropy at level 0 widens
  the footprint along one axis and reads across atlas cells / LUT rows. Its own comment said
  unmipped samplers keep the game's setup; the code did not.
- The scene LOD bias (`TemporalUpscaler::SceneMipBias`, -1 at DLAA since 8fe10a4, log2(r/o)-1 for
  DLSS) is the sampler's `mipLodBias`. Vulkan adds `mipLodBias` to explicit-LOD samples as well
  (`image_sample_l`, `_lz`), so a shader that picks its mip on purpose (blur chains, detail blends,
  lookups) read another level. Decision with the user: keep -1 at DLAA, stop applying it there.

**What the recompiler records** (`Shader::SamplerResource`, shader_recompiler/resource.h; set in
`PatchImageSharp`, resource_patching_pass.cpp, where `ImageSampleRaw` gets
`image_binding | sampler_binding << 16`, so both bindings and the instruction flags are known):
- `explicit_lod`: some sample uses `has_lod || force_level0` without gather or derivatives (the
  same precedence as `PatchImageSampleArgs`); `implicit_lod`: some sample does not.
- `image_mask`: bit per paired image binding; `~0u` (unknown) when a binding is >= 32.
- Merged with OR in `Descriptors::Add`, not part of its dedupe key.
- Info is serialized as raw bytes (`InfoPersistent`), so `ShaderMetaVersion` 13 -> 14 and
  `PipelineKeyVersion` stays 5 (review: keys contain no Info). The legacy meta version (12, no
  binary hash) is no longer accepted: no older meta has this layout. One-time shader cache rebuild
  for players.

**What the renderer does** (vk_rasterizer.cpp `BindSamplers`, `ForceAnisotropy`):
- Bias: a non-depth sampler with `explicit_lod` gets no scene bias (one used both ways neither: the
  safe side). Toggle Disabled2 bit 43 `BiasExplicitLod` (8796093022208), bit set = old behaviour.
- Anisotropy, after the existing checks: skipped when the sampler is used only with an explicit
  LOD, when the S#'s MaxLod is 0, or when every paired image's T# (prepared `image_sharps`, else
  `ImageResource::GetSharp`) has `last_level == base_level`. An unknown mask keeps the old
  behaviour. Toggle Disabled2 bit 44 `AnisoUnmipped` (17592186044416), bit set = old behaviour.
- Both toggles are read live per bind.
- With `GOW3_FRAME_STATS=1` the 5 s printer adds `Samplers (5 s): N scene-biased, M bias skipped
  (explicit LOD, of which X mixed); K aniso raised, J aniso skipped (unmipped U, maxlod0 Z, explicit E)` (counts per
  sampler bind, `Gow3Stats`). The flight recorder is unchanged.

**Review.** Mixed samplers (both `explicit_lod` and `implicit_lod`) lose the bias for their
implicit samples too: the safe side, counted as "of which X mixed" in the Samplers line. The first
start after updating has a cold shader cache (metas 14 only), so it recompiles and stutters like a
new install. `PipelineKeyVersion` stays 5: pipeline keys contain no Info. The sampler counters only
count with frame stats on (`Gow3Stats::enabled`), so players without diagnostics pay no atomics.

**Tests.** No game run. `gow3_pc/build.sh` 0; `ninja -C out/gpu flight-test camera-cut-test` 0,
`flight-test.exe` 0, `camera-cut-test.exe` 0 (unchanged code, regression only). No unit test of the
new fields: Info is written with one raw byte copy, so they round-trip with the struct; checked by
reading `Info::Serialize` / `Deserialize`.

**Not verified.** That the counters show nonzero "bias skipped" and "aniso skipped (unmipped)" in
U3 (chapter 2 market, the player's flash marks of session 20261025-091021); that the edge lines
and the blur are gone (A/B with bits 43/44 and `GOW3_FRAME_DUMP_TRIGGER` dumps at one spot); that
DLAA wall sharpness is kept (Laplacian metric as on 21 Oct); FPS within noise; that the cache
rebuild completes. U1 and U2 not checked. A sampler that pairs with a mipmapped image in one draw
and a one-level image in another is decided per draw from the bound T#s.

## 25 October 2026: lamp shadow passes get the scene jitter

Why: the player reports that with any temporal upscaler (DLSS, TAA, FSR) whole shadows cast by
lamps flicker on and off, in many places, always; with the upscaler off they do not.

**Mechanism** (code reading, same as the sun fix of 19 Oct, #261). The jitter is a viewport shift
(`UpdateViewportScissorState`, `viewport.x = ... + draw_jitter[0]`) given to geometry drawn with the
jittered scene depth (>6 indices or >1 instance) and to full-screen passes only when their pixel
shader is in `GOW3_JITTER_PS` (probe_uncharted.py lists only f614041e, the sun shadow pass). A
full-screen pass that rebuilds world positions from the jittered scene depth with the game's
unjittered matrices samples the shadow map at a position off by up to half a pixel, different every
frame; on surfaces where that exceeds the shadow bias the whole shadow test flips. The lamp (point,
spot) shadow passes are other shaders, so they were never jittered.

**Rule** (vk_rasterizer.cpp `DrawRecord`, after `BindResources`):
- Sampled images: `bound_images` holds the draw's targets and textures; `image.binding.is_bound`
  is set only by `BindTextures` / `BindTexturesFromSet` (reset per draw in `ResetBindings`), so it
  separates sampled images from targets. A sampled depth-format image (`info.props.is_depth`) is the
  scene depth when `JitteredDepth(id)` (the scene depth, or in Uncharted its same-size same-format
  copy), otherwise "another depth image" (a shadow map).
- Depth-compare samplers: `Shader::SamplerResource::is_depth` of the pixel shader (set for the `_C`
  sample opcodes in resource_patching_pass.cpp).
- Shadow pass candidate: samples the jittered scene depth AND (a compare sampler OR another depth
  image). A full-screen candidate (<=6 indices, 1 instance) not already in `GOW3_JITTER_PS` gets the
  scene jitter; logged once per shader: `Upscaler: shadow pass %08x gets the scene jitter (auto)`.
- Full-screen jitter scale: the jitter is in scene pixels; the shift was applied in the pass's own
  pixels, so a pass drawn at half size moved twice as far across the screen as the scene. It is now
  scaled by the viewport size over the scene depth size (`2 * xscale / depth width`), for the
  `GOW3_JITTER_PS` passes too (no change for a full-size pass such as the sun's, if it is full size).
- Toggle: Disabled2 bit 45 `ShadowPassJitter` (35184372088832), read live; bit set = old behaviour
  (no automatic jitter and the unscaled shift). `GOW3_JITTER_PS` and bit 62 `FullScreenPassJitter`
  keep working.

**Diagnostics** (stdout, boot.log; once per pixel shader hash, a small vector of seen hashes; the
per-draw cost is a walk of the draw's bound images while the upscaler is on):
- `Shadow pass candidate: ps .. vs .. indices N instances N jittered D target WxH compare-samplers N
  other-depth D screen-space D` for every candidate draw, full-screen or geometry.
- `Light volume candidate: ...` for jittered geometry (>6 indices or >1 instance) that samples the
  scene depth with a compare sampler. Variant 2: a deferred light volume builds its screen UV from
  FragCoord, which the viewport shift does not move, so it reads depth rendered half a pixel away.
  Only logged, no behaviour change.

**Review notes.** Screen-space passes (clip disabled) ignore `draw_jitter` in the viewport code, so a
candidate with `screen-space 1` is not moved by this change. A shader that samples the scene depth
through a colour (R32F) image registered apart from the depth image is not seen as sampling the
scene depth. A full-screen candidate that is not a shadow pass (fog or volumetric light that tests a
shadow map) would get the jitter too: the log names each one, bit 45 turns it off.

**Tests.** No game run. `gow3_pc/build.sh` 0 (vk_rasterizer.cpp recompiled, no warning on the new
lines); `ninja -C out/gpu flight-test camera-cut-test` 0, `flight-test.exe` 0, `camera-cut-test.exe`
0 (regression only: no unit test covers the draw-time rule).

**Not verified.** That the lamp shadow flicker stops; which shaders the candidate logs name in U1,
U2 and U3; whether the sun pass f614041e draws at full size (the scaling only changes it if not);
whether lamp passes are full-screen or light volumes (variant 2 is not fixed); FPS within noise.

## Sharp text above 1080p (25 Oct 2026)

Request: sharp interface and subtitles at 4K. Where the UI is drawn and why it was soft:
- With the fork's default camera (flat) the game itself runs at the render size the launcher sets (3840x2160 here) and draws its HUD, menus and subtitles into that size after the post chain. The upscaler only reconstructs the HDR scene before the post (SceneTargets, between the camera-depth pass and cs aec539f0), so the DLSS/FSR preset does not lower the UI's resolution. Evidence: U3 pause-menu capture (frame_20261025-130015_1, DLAA): the UI pass #59 draws into the 3840x2160 R8G8B8A8Srgb target. (Code reading for the scaled presets; only DLAA was captured.)
- The softness is the fonts: BMFont bitmap atlases made for 1080p, not distance fields. data.psarc fonts/ps4: U2/U3 main.fnt "Adelon-Serial DB" size 42 (U2 256x128, U3 256x256 RGBA, `packed=1`: one glyph per channel), U1 game3.fnt (same face, 256x128), plus symbol3/chinese/japanese/korean/russian. The cached U3 atlas (0x1148da8000, 256x256 R8G8B8A8) is byte-identical to main_00.tga. At 4K the game draws the text 2x larger: "Options" caps about 70 px from about 31-texel glyphs, so bilinear spreads each edge over 3-4 pixels (4K display dump).
- Text shader ps 2e403aa4 (same hash in the U1, U2 and U3 shader caches; vs ad649c68): four atlas reads per pixel (the pixel, -0.375 and +0.375 pixel along x from quad derivatives for subpixel colour: R from the right read, G centre, B left, and a drop-shadow read 1.5 pixels up-left), channel picked by a per-vertex mask, alpha = vertex alpha x max(mean of the three, shadow x shadow opacity).

Options considered: a larger atlas would need the texture cache to back a 256x256 T# with a bigger image (views, size checks, uploads) and glyph art the port cannot ship; drawing the UI after the upscaler is not needed (it is already at the game's size). Chosen: a shader-side remap, no new assets.

Fix (shader_recompiler/ir/passes/text_sharpen_pass.cpp, after ResourcePatchingPass): in shaders listed by GOW3_TEXT_SHARPEN (default 2e403aa4; 0 = off; hex list), every ImageSampleImplicitLod result c becomes clamp((c - 0.5) x k + 0.5, 0, 1) per channel, k = max(1, 2^-lod) with lod the unclamped ImageQueryLod at the same coordinates (pixels per texel). Edges get back to about one pixel; at one texel per pixel or less (1080p and below) k = 1 and the output is unchanged. All four reads get it, so the shadow and the subpixel fringes keep their 1080p proportions. The SPIR-V cache name gains `_txt1` for these shaders (vk_pipeline_serialization.cpp), so cached unpatched binaries are not reused; with GOW3_TEXT_SHARPEN=0 the old names (and binaries) apply.

Measured (worktree build, player's settings: 4K, DLAA model M, 120 cap, MT on; GOW3_FRAME_DUMP_TRIGGER display dumps 3840x2160; same exe with GOW3_TEXT_SHARPEN=0 as before; share of pixels between 20% and 80% of the crop's 5-95% luma range = soft edge pixels):
- U3 pause menu, text-only crops on the menu panel: "Save Game" 3.3% -> 1.6%, "less than 1 minute ago" 5.3% -> 2.8%; mean gradient 3.34 -> 3.61, 6.89 -> 7.24.
- U3 pub tutorial hint (bottom-centre, subtitle position, same shader; reached by the same inputs in both runs): "To throw a nearby" 20.6% -> 16.0%, "holding" 21.1% -> 14.2%, "while" 37.1% -> 33.3% (these crops include the busy floor behind the text). At 3x zoom the before edges are 3-4 px ramps, after about 1 px.
- U2 and U1 pause menus: the patched shader is built in all three caches (`0x000000002e403aa4_0_ldsb_txt1.spv`); text renders normally. U1 still renders at 2880x1620 at a 4K setting (its render-target heap limit), so its UI is sharper in the 1620p image but then stretched to the window.
- The patched SPIR-V (spirv-cross): `max(exp2(-textureQueryLod(...).y), 1.0)` per read, then the clamp(fma) remap.

Not verified: a real cutscene subtitle (the hint text, drawn by the same shader at the subtitle position, stands in); 1440p; scaled DLSS/FSR presets (code reading says the UI stays at the game's size); performance (four extra LOD queries in a shader that covers only the text; not measured); a U3 boot stall seen once (live view stopped at frame 147 during boot, with GOW3_TEXT_SHARPEN=0, so the unpatched shader path; the retry ran normally). Not changed: the button icons inside text (symbol3 atlas, ps 4ad58d25) and other HUD pictures, which are colour images, not coverage.

## 22 October 2026: phase 5c, A-to-B draw pipe measurements

Diagnostic only, no behaviour change. Commit 70e0627 adds `GOW3_DRAWREC_STATS=1` (off by default, independent of `GOW3_FRAME_STATS`): counters in `DrawPipe` (vk_draw_pipe.h) and `Submission` new/reuse counters (vk_draw_prep.cpp), reported as two lines (`DrawRec A:`, `DrawRec B:`) about every 5 s. Stage A counters are plain members (the GPU command thread is the only producer); stage B publishes relaxed atomics in batches; none of them takes part in the queue's control flow, the sleep protocol and every memory order are untouched. Time unit is TSC ticks (not ordered, can include preemption); percentages are of the window's elapsed ticks.

Setup (22 Oct, U3 chapter 2 meat market from `C:\dev\bench\backup_ch2meat`, 1080p, DLSS model K, player's other settings, `tools/play.py measure` 10 s windows, build 70e0627 from this worktree, full `build.sh` exit 0). The default `bench.sh` save is a lighter scene capped at 120 FPS with about 435 draws a frame: six windows there were discarded.

Counter coherence (ON1 19 windows, ON2 32 windows of ~5 s):
- Exact (A-side cumulative, window deltas): byte residue 0 in every window checked (payload + header + slack + pad64 + wrap = physical advance of `head`); no non-finite values printed.
- Sampled: depth after each Commit (mean, window max, both histograms; not the time distribution of occupancy). Max depth 0.87 MiB of 16 MiB.
- Approximate: pending packets (A's count minus B's batched count): median about 2, mean 79 (spikes to 745).
- Unavailable: B's percentage per window (printed `unavailable`, not zero) and B's maxima per window (printed as since start).

Measured, ON1 (19 windows, about 93 FPS, p50 10.6 ms, p99 13.1 ms):
- 4,560 packets a frame (about 425,000/s): about 700 draw, 2,006 task, 1,854 dispatch, 5 special, 8 indirect. Payload per packet: draw 1.7 KiB, dispatch 0.9 KiB, task 48 B.
- 281 MB/s physical, about 3 MB a frame: 96.5% payload, 1.2% header, 1.8% pad64, about 0 wrap, 0 slack.
- Depth after Commit: mean 89 KB, 100% of commits in (0, 25%] of capacity; fine histogram 42% <= 4 KiB, 26% <= 64 KiB, 32% <= 1 MiB, 0% above. Space waits: 0.
- Stage A write path (Begin to Commit minus WaitForSpace): about 307 ticks a packet, 3.7% of the window. It excludes the preparation before Begin (CollectRingBindings and others) and includes preemption.
- Drains: about 4,027/s (about 43 a frame), 3,460/s waited (86%), 567/s found the pipe empty. Post-drain-to-commit gap: about 3,460 observations/s, mean 22 us (overlapping intervals after consecutive drains, not exclusive time, not "A idle"); typical window max 1-2 ms, two windows 20-22 ms.
- Stage B completed intervals: idle spin about 149 ms/s (14.9%), wake.wait about 37 ms/s (3.7%), so about 18.6% of the time idle; this agrees with "stage B busy 81.4%" of the earlier trace. About 115 sleeps/s, 115 wake returns/s (no skipped waits), 446 notify calls/s.
- Submission: about 2,570 built/s, all new allocations, 0 pool reuses (`GOW3_PREP_POOL` was off).

Overhead of the diagnostic (stats ON vs OFF; the gate is read once at start, so each state is a separate session; order ON1, OFF1, ON2, OFF2; 6 windows of 10 s each; port FPS):
- Session means: ON1 92.80, OFF1 89.93, ON2 95.05, OFF2 95.62 FPS (within-session SD 0.3-0.8).
- Pooled ON 93.92, OFF 92.77 (+1.15). Pair differences ON - OFF: +2.87 and -0.57. p50 10.52 vs 10.61 ms; p99 12.96 vs 13.65 ms.
- No slowdown is visible, but the design cannot prove an overhead under 1 FPS: the two OFF sessions differ by 5.7 FPS, more than any effect seen. More alternating sessions would be needed for a bound.

Reading (hypotheses, not measured as causes):
- The queue is never full and stage B keeps up (pending about 2, B idle about 18.6%): the ring capacity is not the limit, and packet size is not either (write path 3.7% of A, under the 5% line that would justify compacting records).
- Stage A is interrupted often: about 37 drains a frame wait for stage B to run dry. This matches the 22 Oct wait trace (about 3,400 slow-path fence drains/s, about 190 fault-flush commands/s). The drains-to-empty cost stage A about 18.5% of its time in that trace.
- The `Submission` pool was not exercised; its effect is unknown.

Not verified: other scenes and chapters, U1 and U2, 4K, a long soak; the diagnostic's overhead below 1 FPS; B's per-window percentage. Four sessions, one machine (Ryzen 5 5600), logs kept outside the repository in `C:\dev\uncharted\analysis\d5c_logs`.

Next step (proposed, not started): cut the waits of stage A on the pipe, starting with the deferred-fence drains.
1. Land the fence tick as an in-order pipe task (`RunInOrder`) so stage A waits for that task, not for the whole queue (`LandDeferredFenceFor`, about 9.4% of A in the 22 Oct trace; upper bound if both drain waits vanished: A 21.8% shorter, real gain smaller).
2. Behind a new `Disabled2` bit (read live, bit set = old behaviour) so it can be A/B'd in one session with `ab2.sh` at 1080p and 4K, DLAA K, meat market, at least 6 + 6 windows.
3. Risk to test first: stage B owns the readbacks while it runs (the 18 Oct race, `GOW3_READBACK_RACE`), so stage A must not land ticks beside it. Test with the readback race check, `GOW3_STAGEA_VERIFY`-style equivalence counters, and the floating-NPC / U2 cases from 16 Oct before any claim.
4. Re-run `GOW3_DRAWREC_STATS=1` afterwards: waited drains a frame, post-drain-to-commit and B idle should move; FPS gain must beat the run-to-run noise measured above.
5. Smaller, independent: try `GOW3_PREP_POOL=1` once with the same A/B (about 2,570 allocations/s today).

## 24 October 2026 (night): U1 treasure models, synchronous shaders again

- Symptom (user report): since 0.3.0 (U1 back to async shader compilation) the treasure models (Treasures screen, pickup) do not appear.
- Static trace: the viewer scene update (0x2061e8) and render (0x206b58) run every frame, so only a compile that never completes or fails can hide the model.
- In 0.3.0, PublishGraphics leaves a failed async pipeline in pending_graphics forever. On main, failed pipelines/shaders go to failed_graphics / compilation_failed / failed_permutations and the async branch never queues them again (vk_pipeline_cache.cpp GetGraphicsPipeline, GetProgram). Compute is never compiled async (compile_async excludes Compute). Either way the draw is skipped for good.
- None of the 86 U1 logs has a "compilation failed" / "capture failed" line, but none ever opened a non-default treasure, so this is not ruled out.
- Fix: U1 compiles synchronously by default (0.2.3 behaviour). Cost: the 1-2.4 s new-area stalls on laptops (18 Oct). UNCHARTED_U1_SYNC_SHADERS=0 restores async_shaders.
- If a log shows "compilation failed" lines, the root fix is: a failed async graphics pipeline / program falls back to a synchronous compile.
- Other candidates left (not tested): GOW3_COMPUTE_PRECOMPILE=0, GOW3_READBACK_GUARD=0.
- Not tested: no in-game run (user asked for none).

## Launcher redesign and 4K-sharp drawing (25 Oct 2026)

Player-facing wording: CHANGELOG, Unreleased, 25 Oct 2026. Two mockups were drawn in VectorCraft
(A "expedition journal", B "cinematic"); the user picked B.

What changed:

- Layout (uncharted_launcher.pyw build_stage / layout / render_stage): one canvas (the stage) fills
  the window. Its backdrop is a Pillow image redrawn on resize (60 ms debounce) and on a game
  switch: pic1.png (or pic0.png) of the player's game, blurred at quarter size, 98% dark on the
  left to 57% on the right, the selected game's colour as a radial glow, and a film grain
  (launcher/assets/grain.png, a 512 px tile made in PhotoCraft: add noise 18, gaussian,
  monochromatic, blur 0.4). The tab bar, GitHub, the section panel, the PLAY panel and a halo
  round the selected game card are drawn into it. Tabs, cards, PLAY, UPDATE and GitHub are small
  canvases placed on the stage; each shows its crop of the backdrop (backdrop()), so the glass
  reads as see-through. The section panel is a Frame inset by the corner radius, so the backdrop's
  rounded corners show. The banner and the sidebar are gone; the game's name moved under the
  section title (section_sub). Status and substatus sit under PLAY and wrap.
- Sizes: default 1280x780, minimum width = margins + 264 px column + 880 px body (fit_to_screen).
  The Controls grid's controller column went from 14 to 8 characters (an icon) and the action
  names from 17 to 15, so both columns fit 880 px.
- Per-game accent (GAMES "accent": #e6b44c, #e5553f, #f08c2a): widgets.set_accent swaps every Tk
  option holding the old accent (bg, fg, highlight, active, select) and the module ACCENT globals;
  widgets read ACCENT when they draw; select_game calls apply_accent before refresh_controls.
- 4K-sharp drawing (launcher/art.py): rounded rectangles drawn 4x and reduced with LANCZOS
  (rounded_image, lru_cache of 96: the stage's big panels come back on each game switch), icons
  from 192 px white masters tinted and reduced (icon). Tk's smoothed polygons (round_rect) are no
  longer used. Toggle, Chip, Segmented (now a canvas tray with one Label per option, its pill as
  the Label's image), Dropdown, ScrollBar, Badge, InfoTip, DialogButton, Dialog icons, Toast and the
  preset cards use them. Toggle, Chip, Segmented and Dropdown draw their focus ring as a rounded
  outline. Fonts are vector, so text was already sharp.
- Icon set: 18 icons on a 24 pt grid, 1.8 pt round strokes, drawn in VectorCraft;
  sources launcher/assets/icons/src/*.svg, masters launcher/assets/icons/*.png (192 px = 8x).
  App icon redrawn in VectorCraft (assets/app-icon.svg, 1024 px app-icon.png);
  tools/make_launcher_icon.py now reduces that master to uncharted.ico (16-256) and icon.png (512).
- Game art: art.trophy_file reads sce_sys/trophy/trophy0N.trp of the player's game (header 0x60
  bytes, count at 0x10, entry size at 0x14, entries name[32] + offset/size u64 BE; the PNGs are
  not encrypted). GR001.PNG (320x176, the game's coin) is cropped to its alpha and enlarged with
  LANCZOS plus an unsharp mask when the factor is over 1.2. Nothing of the game is packaged;
  without a game folder the card shows a drawn medallion with the game's number.
- Dialog.show returns at once if the dialog was answered during its first update() (the UI test's
  100 ms timer could fire there once building the dialog got slower).
- PhotoCraft had no file access in this session (read and write authority absent), so only the
  grain tile came from it (through its preview); the game-art upscale is done in Pillow at run time.

Measured (1x, this PC): backdrop 58 ms at 1280x780, 183 ms at 2560x1560; a game switch 140-210 ms
(was ~420 ms before caching the panels).

Tested: gow3_pc/tests test_uncharted_launcher_ui, 40 tests (3 new: trophy pack reader with a
synthetic pack, icon tint and size, set_accent). Screenshots of every section for U1/U2/U3 at
100% and of Home at 200% (tk scaling 2; window capped by the 2560x1440 screen), and without a
game folder. Tab order: games, tabs, panel, PLAY, GitHub.

Not verified: a real 3840x2160 monitor at 150%/200% Windows scaling (only Tk scaling here); a
controller driving the new layout; the UPDATE pill with a real release; the packaged zip;
Wine/Proton (Bahnschrift missing falls back to Segoe UI).
