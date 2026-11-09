# Audio blocker (SGX `tverb`) — root-cause analysis

## Symptom
The recompiled game boots fully (fonts, trophies, save, network, 12 SPU jobs,
Vulkan clear) and then tears down SGX and exits:

    *** sgxbus.c(934)  : failed to regist module
    *** sgxbus.c(1634) : unknown module name (tverb)

The upstream (SilvioDiaz) documents the same wall in `docs/WHERE_IT_STOPS.md`
(2069 lines) and has **not** solved it (Phase 5 "Audio — NOT STARTED").

## What was established here

1. The SGX bus-count query is commands **0x105 (input buses) / 0x106 (output
   buses)**, answered per DSP module. The `dspjob` (image 2) dispatch routes
   them to `func_00001244`, which sets the answer to **1**:
   `gpr[5] = spu_il(1)`.

2. That answer must reach the PPU through the SPU's **outbound mailbox**
   (`wrch $MBOX` → `spu_wrch(ctx, SPU_WrOutMbox, …)`), then `jc_signal_done`
   forwards it as the completion event's `data3` for the 0x105/0x106 range
   (the `answers_a_count ? mbox : 0` logic in `libs/spurs/cellSpurs.c`).

3. In this JP run the chain is broken in **two** places:

   - **Command encoding mismatch.** The PPU-side diagnostic
     `g_spurs_job_cmd = (g32(g32(job_ea+0x4C)) >> 16)` does not always equal the
     command the SPU actually dispatches on (`gpr[13]`). The SPU reads the
     command through the descriptor's command-block pointer inside the LS, and
     the block is not guaranteed to be the same 32-bit word the PPU side reads.

   - **The outbound mailbox is drained before it is read.** Instrumenting
     `func_00001244` shows the SPU *does* post `1` to the outbox, but the
     post-run read in `spurs_job.c` sees the channel empty — a later kernel
     re-entry (the job's "return to LS 0x3FF00" second lap) consumes the word.

## Fix direction (for whoever continues)

- Make the SPU→PPU answer channel deterministic for 0x105/0x106: capture the
  outbox value at write time (in `spu_wrch`) rather than after the run, gated to
  the bus-count queries, and reconcile the command the SPU dispatches on with
  the command `cellSpurs.c` gates the forward on.
- The deeper/cleaner fix is **resident SPU threads** (the `write_spu_mb`
  handler already documents this as the real fix) so the mailbox handshake is a
  live worker, not a re-run whose state is lost between calls.

## Status
Everything up to and including Vulkan rendering + the 12 SPU job images is
working and committed. Audio output is the remaining hard gate, and it is an
architectural ps3recomp change, not a game-side patch.

## Follow-up: post-patch Linux fixes (after the patch applied)

Applying the patch fixed the SGX audio (no more `sgxbus tverb`). Three more
Linux-specific fixes were needed on top of it:

1. **`std::bad_alloc` at boot ctors** — the PPU code was re-lifted with the
   patched `ppu_lifter.py`, which regenerates `generated/ppu_recomp_*.cpp` and
   **wipes the post-lift heap redirects**. Re-run `scripts/post_lift.py` after
   any re-lift (`analyze.py` does this, but a manual `ppu_lifter.py` run does
   not). Symptom: `exception: bad allocation` right after TLS init.

2. **Flip-wait spin** — the game's wait-for-flip loop (`func_001F18D4` polling
   `cellGcmGetFlipStatus`) spun forever because the D3D12 presenter is stubbed
   on Linux, so the flip stayed `WAITING`. Fix in
   `ps3recomp/libs/video/cellGcmSys.c` (non-Windows): auto-complete a WAITING
   flip to DONE inside `cellGcmGetFlipStatus`. Runs unpaced until a real Vulkan
   present lands.

3. **Font crash** (open): after `cellFontOpenFontset`, a font import dereferences
   a near-NULL pointer (fault 0x0000000B) in `func_0025BD80` (import slot
   0x342464). Next target.

## Follow-up 2: font crash + SPURS bring-up (next frontier)

The ps3recomp `libs/font/cellFont.c` (d7fff845 base) treats its guest-pointer
arguments as HOST pointers (`fontType->type`), but the HLE dispatch is a
generic adapter that passes RAW gpr values — so `cellFontOpenFontset` dereferences
the guest EA directly and SIGSEGVs (fault 0x0000000B). The v0.19.0 cellFont.c
used `FONT_EA(p) + vm_read32` (guest-EA access) and was correct.

Workaround (not a fix): `TJ_KEEP=cellFont*` keeps the font imports on TJ's own
stubs (return 0) so the boot continues. With that, the boot reaches the SPURS
(audio) bring-up — `cellSpursInitializeWithAttribute`, `cellSpursCreateJobChainWithAttribute`,
`cellSpursRunJobChain`, `cellSpursJobGuardNotify` — then spins in
`cellSpursJobGuardNotify(guard=0x3)` (unaligned -> CELL_SPURS_JOB_ERROR_ALIGN).
The job-guard pointer reaching the PPU is 0x3 (corrupted); next target.

## Follow-up 3: SPURS job-guard corruption (guard=0x3)

With `TJ_KEEP=cellFont*`, the boot reaches the real SPURS bring-up and runs the
first job chain successfully:

```
[cellSpurs] Initialize "sgx-spurs-" ea=0x00C07500 nSpus=1
[cellSpurs] CreateJobChainWithAttribute(jc=0x0264A080 ... name="soc-job")
[cellSpurs] JobGuardInitialize(guard=0x0264A200 jc=0x0264A080 count=1 spus=1 auto=1)
[cellSpurs] RunJobChain(jc=0x0264A080) run#1
[cellSpurs] JobGuardNotify(guard=0x0264A200) reached zero
```

Then the frame loop spins in `cellSpursJobGuardNotify(guard=0x3)` — the guard EA
reaching the PPU is 0x3 (near-NULL, unaligned) so it returns
`CELL_SPURS_JOB_ERROR_ALIGN` and the guest retries forever. The valid guard is
0x0264A200 (= jc 0x0264A080 + 0x180), so the guest's job-chain/guard pointer is
being corrupted to 0x3 somewhere in the SPURS job-chain publish/notify path.
Next target: trace where the guest's `gpr[26]` (= jc + 0x180) becomes 0x3.

## Follow-up 4: main loop blocks on SYS_EVENT_QUEUE_RECEIVE (queue #3)

The soc-job runs (guard reaches zero), then the main thread's job-walk loop at
`func_0020D6D0` reaches `lv2_syscall` with gpr[11]=130 = `SYS_EVENT_QUEUE_RECEIVE`
and r3 = queue id 3 (the SPURS dynamic queue, port 16). It blocks there forever
— the SPU job completion event is never delivered.

`cellSpursDeliverSpuEvent()` (the patch's SPU interrupt-mailbox -> LV2 queue
forwarder) exists, but it is only triggered when a lifted SPURS job writes its
outbound mailbox. The soc-job guard reaching zero is PPU-side only
(cellSpursJobGuardNotify); the SPU-side completion signal (the outbound-mailbox
event) is what's missing. Next: verify the socjob/dspjob SPU images actually run
and post their outbound mailbox.

## Follow-up 5: SPU jobs now RUN (jc_execute) + AudioOut fixed (GUEST_PTR)

Two more Linux-only fixes:

1. **Job chains never executed on Linux.** `cellSpurs.c` `jc_run_chain()` spawns
   the async `jc_thread` worker only under `_WIN32`; the `#ifndef _WIN32` path
   just unlocked and returned without running anything. So the soc-job's SPU
   code never executed, never posted its outbound mailbox, and
   `cellSpursDeliverSpuEvent()` never fired → the main loop blocked forever on
   `SYS_EVENT_QUEUE_RECEIVE(queue #3)`. Fix: call `jc_execute(i)` synchronously
   in the non-Windows path. Result: `SPU event port=16 ... -> queue=3 rc=0`
   now flows, and the boot advances past the event-queue wait.

2. **cellAvconfExt host-pointer bug.** `cellAudioOutGetDeviceInfo` and friends
   dereferenced their out-params as HOST pointers (`info->portType`) while the
   HLE dispatch passes RAW guest EAs (generic uint64 adapter). SIGSEGV at fault
   0x0B. Fix: add the v0.19.0 `GUEST_PTR(p,T)` macro (`vm_base + (u32)p`) and map
   each out-param before use; `cellVideoOutGetGamma` uses `vm_write32` for the
   byte-swapped float.

Current next blocker: the SGX audio lib (`sgxps3ps3.c:1820`) reports
`failed to set notify queue (0x23A0)` right after `cellAudioSetNotifyEventQueue`
is expected to register the queue key, then `failed to DI/EI (0xC07438)` and the
game tears down (unregister callback, delete SOC job, terminate SPURS).

## Follow-up 6: SGX "set notify queue" = NULL function-pointer call

Traced the failure with `PS3_HLE_TRACE` + `YDKJ_SCTRACE` + `PS3_CALLTRACE`:

- `cellAudioSetNotifyEventQueue` (NID 0x377E0CD9) is NEVER reached (no HLETRACE
  for it).
- The syscall right after `queue_create(key=0x23A0)` is just `SYS_TTY_WRITE`
  (the error print) — so "set notify queue" is a DIRECT guest call, not an
  import/syscall.
- `PS3_CALLTRACE` shows the failure is a sequence of `[CALL] -> 0x00000000`
  (indirect call to a NULL function pointer, r3=0x00C0D138). The runtime's
  NULL-ctr sentinel just returns, leaving r3 non-zero, so the game prints
  "failed" and tears down.

So the SGX audio lib's API function-pointer (its notify-queue hook, or the
import OPD for cellAudioSetNotifyEventQueue at slot 0x3423EC) is NULL at that
point. Next: find why that pointer is NULL (import slot not resolved, or the
SGX API table not populated during its init).

## Follow-up 7: audio graph now LOADS the sound banks

Four more root-cause fixes (all the same host-pointer/guest-EA family):

1. **Audio port buffer collided with the HLE OPD area.** `AUDIO_GUEST_DEFAULT_BASE`
   was 0x01000000 = HLE_OPD_BASE; `cellAudioPortOpen`'s buffer memset wiped every
   import OPD mid-boot (cellAudioSetNotifyEventQueue's OPD fn went 0x011802DC ->
   0). Moved to 0x01400000. The SGX "set notify queue" / DI / EI failures
   disappeared and the sgx-audio thread starts.

2. **cellSail host-pointer bug** (cellSailMemAllocatorInitialize/GetState/
   GetStreamNum): added GUEST_PTR mapping.

3. **cellFs host-pointer bug**: the whole cellFs.c treated guest paths as host
   strings (`printf %s`, `strncmp` on raw EA) and out-params as host pointers.
   Added gpath()/gptr() and mapped path/fd/sb/buf/pos/entries.

4. **cellFs bare device root**: `/dev_hdd0` never matched its own `/dev_hdd0/`
   mapping, so the game's "stat the device" looped forever; accept the prefix
   with the trailing slash removed.

Result: the game now OPENS AND READS the actual music banks —
`data/snd/static.sgd`, `20_tj_result.sgd`, `05_tj_stage02.sgd` — and the
sgx-audio thread runs. Next blocker: the main thread blocks in
`cellSpursEventFlagWait(flagEA=0x005F7400 pattern=0x1)` forever — the SPU job
completion event flag is never set.

## Follow-up 8: 3 more SPU job images captured + lifted; boot reaches the GCM render loop

The 2nd SPURS instance dispatches THREE more job images that were never
captured: 0x78FEA84E1C23BB5C (6.3KB), 0xD8945A0BA2D3D054 (40KB) and
0xDD6F7BFDDFCF7894 (48KB). Captured with SPU_DUMP_MISS, lifted with the patched
spu_lifter.py (images 13/14/15 in spu_workloads.c). Also simulated the SPU
kernel "ready" event-flag (bit 0 set at EventFlagAttachLv2EventQueue, non-Windows).

Result: no more dispatch MISSes; the sgx-audio threads run; ALL the sound banks
load (static.sgd, tj_bus.sgd, 20_tj_result.sgd, 05_tj_stage02.sgd); the save-data
auto-load check runs. The boot now reaches the RENDER LOOP: the main thread
spins on GCM control register 0x03002008 (=0) waiting for the RSX pump — the
graphics frontier (D3D12 stub -> needs a real present/Vulkan).

## Follow-up 9: boot reaches the RENDER LOOP (GCM fence wait)

Made the RSX FIFO pump (`cellGcm_rsx_process_fifo`) unconditional in
tj_present.cpp (it was gated behind `rsx_ok`, which is 0 because the D3D12
backend is stubbed). The audio side is now fully up: all sound banks load, the
sgx-audio threads run, the SPU jobs (15 images) dispatch.

The main thread now sits in the game's render loop spinning on GCM control
register 0x03002008 (the REF/fence register) = 0. The fence queue is empty:
`gcm_ref_push_at` fills it only when the FIFO drain consumes SET_REFERENCE
commands, and the drain has nothing to consume yet. This is the GRAPHICS
frontier: the guest must be able to write its command buffer and have the RSX
walk it — with the D3D12 renderer stubbed, the geometry never executes. Next
big piece: a real present/renderer (Vulkan) or at minimum a stub that walks
the FIFO and publishes fences.

## Follow-up 10: REF hack gets the render loop RUNNING (fonts + DLC load)

The game's gcmFinish-style spin (func_001F734C: polls ctrl->ref 0x03002008,
usleep(30)) never terminates because the game's FIFO carries no SET_REFERENCE
(method 0x50) the drain can publish. Hack (TJ_REF_HACK=1): sys_timer_usleep
detects the spin's usleep(30) (lr 0x001F7384/0x001F739C) and writes the
expected fence (gpr[31]) straight to ctrl->ref.

Result: the render loop advances — fences 1..7+ publish, PFontThread runs,
DLC edats are probed (dlc0008..dlc1008), fonts bind/render/close. Remaining
stall: the FIFO ring can't recycle — drain GET sits at 0x12288 on a bind
(SET_OBJECT, count=0) and the packet walker's `remaining = count` convention
appears off-by-one for the 1-data-word bind, so the object handle is never
consumed. That's the next FIFO-walker fix.

## Follow-up 11: render loop runs; FIFO walker drifts into the io-0 header region

With TJ_REF_HACK the fences keep publishing (refs 1..8+ observed) and the game
keeps rendering: fonts bind/unbind/close, DLC probing, ring recycle callbacks.
The walker's remaining problem: its GET starts at io offset 0 and treats the
ring's header word (0x00000001 at io 0, then zero padding) as SET_OBJECT binds,
so it drifts into unmapped io (recoveries "GET 0x14766570/0x1F333330 unmapped;
skipped to CALL/PUT"). Next: start the walker at the game's real ring base and
handle the ring tail JUMP so the walker stays inside the ring lap.

## Follow-up 12: ring now advances; walker executes the game's draw CALLs

Four FIFO-walker fixes in cellGcmSys.c (all gated/observable, upstream file):
1. walker GET starts at context.begin (was io 0 -> misread the game's header
   region as SET_OBJECTs and drifted into unmapped io);
2. inner loops read data words only while getoff < put;
3. outer loop bounded by the ring end;
4. PUT-rewind resync: when the game's window flush rewinds PUT behind GET,
   drop the stale packet and resync (fired 2x, the window then advanced
   0x106DC -> 0x107A4).

The game's command ring now advances and the walker reaches the game's real
draw stream: a libgcm CALL into a heap subroutine (target 0xF2BDF0, return
0x107A8). That subroutine (the game's inline draw commands) is the current
stall -- the walker is inside the CALL and the RET hasn't completed. Next:
walk the CALL subroutines to completion (the game's draw data is in the heap,
already mapped), i.e. make sure every subroutine ends in RET (0x00020000) and
the walker's CALL/RET state machine doesn't wedge on the subroutine's tail.

## Follow-up 13: count+1 unlocks the real command stream (label fences execute!)

The GCM method header's count = num_data - 1. The packet walker used
`remaining = count` (off by one), so every count=0 command (SET_OBJECT binds
among them) skipped its data word and the walker drifted. With
`remaining = count + 1` (both walkers) the game's REAL stream executes:

    [RSX] label write @0x03022000 = 0x00F2C5F1 (sync fence, ...)

and the window advances to 0x686E8. Remaining stall: a CALL whose target
0x1F800000 (io) is unmapped -- that's the game's framebuffer ea 0x3F800000.
The game's libgcm CALLs into inline draw data there; the io table doesn't map
that page. Next: map the framebuffer region in the io table (or bound the CALL
target) so the subroutine walk completes and RETs to the ring.

## Follow-up 14: CALL/JUMP recovery works; walker now in the draw stream

Bound the outer walker's ring-end check so CALL subroutines can run past it.
The walker now recovers from the unmapped VRAM CALL target cleanly
("GET 0x1F800000 unmapped; skipped to 0x68700 (left CALL)") and keeps going.
Current state: the sync-fence label writes execute repeatedly, and the walker
advances through CALL subroutines (getoff reaches 0x69F08). New stall: inside
a CALL (return 0x684AC) a 12-word packet's data is 0xBF800000 (float -1.0f) --
the game's inline float/vertex data in the stream. The packet data walk needs
to keep consuming method data without wedging on float payloads.

## Follow-up 15: game-label semaphores consumed; walker reaches the ring tail

The NV406E semaphore frontend rejected the game's own labels (dma=0xC3B40000,
offset=0x3F000000 — the game's VRAM labels, not the 0x66606660 magic) and
wedged the packet on the ACQUIRE. With the stub GPU the work completes
instantly, so those acquire/release are now consumed (return 1).

Result: the walker crosses the semaphores and reaches the ring tail
(get=0x904DC). Remaining stall: a 247-word packet whose data walks past the
ring end (word at get = 0x200104E0, type 1 — the game's ring-wrap marker or a
misread header). Next: handle the ring tail marker (the game's own wrap word)
or bound the packet at the context end.

## Follow-up 16: walker reaches the game's inline draw blobs (deep in the stream)

After the semaphore consume, the walker crosses into the game's inline draw
data: large method packets (576-word blobs of embedded method headers, e.g.
0x00040064 = the semaphore-offset method) inside CALL subroutines. The walker
now stalls at the game's PUT (get==put, mid-packet) while the game waits for
the ring to drain: the game writes its inline command blobs incrementally and
the walker's packet needs the rest of the blob before the PUT advances.
Remaining work is the blob/ring handshake: treat a packet whose data crosses
the live PUT as complete (flush the partial packet) or drain the blob in
lockstep with the game's writes.

## Follow-up 17: count = num_data (reverted count+1); PUT advances to 0x18AA4

The game's method count is num_data (verified against its semaphore methods);
the count+1 over-consumed and misread the inline blobs. Reverted to
`remaining = count` with SET_OBJECT (method 0) special-cased to consume its
one handle word. The PUT now advances far (0x18AA4+), CALLs recover cleanly
from unmapped targets (0x1ECCCCCC, 0x1F333330), and the game-label semaphores
consume. Current stall: a VALID NV406E ACQUIRE (dma=0x66616661, offset=0x400)
waits for label 0x03000400 == 1, actual 0. The label is libgcm's reserved flip
semaphore -- the game's PPU side (cellGcmSetLabel / flip path) should publish
1. Next: make the PPU-side label writes reach 0x03000400 (or satisfy the
ACQUIRE when the backend is a stub).

## Follow-up 18: title-screen LOGIC runs (text glyphs, saves, trophies)

The ACQUIRE satisfaction (publish the expected label after the bounded wait)
unblocked the flip sync. The walker now runs the render loop continuously:
2000+ unmapped-CALL recoveries, PUT-rewind resyncs, and the game advances
through: save data LOAD (BCJS30069-AUTO-, SYS-DATA 228832 bytes), the trophy
context, all the DLC probes, and then the TITLE SCREEN text rendering --
PFontThread + font glyph calls with Shift-JIS character codes (0x7A7A, 0x306E,
0x3082...) via the TJ_KEEP stubs (no glyph pixels yet, but the game's title
logic is executing). No faults. Next: real font glyphs (cellFont render
path), then a present/renderer to show the frame.

## Follow-up 19: SOFTWARE CLEAR + framebuffer dump — the frame is REAL

Two additions turn the stub GPU into a software frame:
1. rsx_commands.c CLEAR_SURFACE: when the backend is NULL (Linux), paint the
   guest's local-memory render target with the clear color (1280x720 / the
   scissor height, pitch from the surface state).
2. tj_present.cpp TJ_FBDUMP=/path: every 2 s dump the display buffer
   (0xC0000000, 1280x720, pitch 5120) as a raw frame.

The game's clears now execute for real: 386x white (0x00FFFFFF) + 14x dark
gray (0x00333333) into 0xC0000000/0xC03A0000 — the game's title/loading
screens. The dumped frame is the game's actual rendered frame (uniform white,
glyphs still missing because cellFont is stubbed). This is the first time the
port produces real pixels from the game's command stream. Next: software
NV0039 blits + real cellFont glyphs to get the title text visible.

## Follow-up 20: REAL cellFont (no more TJ_KEEP) + software frame pipeline

Ported the guest-EA conversion to cellFont.c (GUEST_PTR + vm_read/vm_write for
the font/fontType/layout/metrics/surface/image fields), added the 5 missing
NID-table entries (cellFontOpenFontInstance/SetScalePoint/SetResolutionDpi/
GetCharGlyphMetrics/GetFontIdCode), and added the GUEST_PTR macro to
ppu_memory.h. The game now runs WITHOUT TJ_KEEP=cellFont*: cellFontOpenFontset
(type 0x100) executes, the glyph rendering path is guest-EA-safe, no crash.
The glyphs go into the game's font surface; the last step for visible text is
the draw/blit from that surface to the framebuffer (still stubbed).

## Follow-up 21: 2D blits (m2mf/scaled) confirmed live; draws are the last gap

The NV0039 m2mf and NV3089 scaled blits already execute (guest-EA aware), and
the game's inline GCM2D writes land in the local memory (0xC0AE0700 region --
the game's UI surfaces). The frame is still uniform white because the
NV4097 draw calls (s_backend->draw_arrays = NULL on Linux) never rasterize the
glyph quads into the display buffer. Remaining for visible text: a software
draw_arrays (textured quads from the game's vertex stream + the font surface),
or at minimum a surface-to-display copy for the game's UI plane.

## Follow-up 22: real TTF glyph pipeline (lazy init + host fallback + renderer fix)

The game's font init goes through the FreeType-style entries (never the plain
cellFontInit), so s_font_initialized stayed 0 and OpenFontset bailed early
(empty glyphs). Fixes: lazy font init in OpenFontset, a host TTF fallback
(DejaVuSans, 759720 bytes -> stb_valid), and vm_write32 for the
cellFontCreateRenderer out-param (was *renderer = i on a guest EA -> SIGSEGV).
No crash now; the glyph path is live. The glyphs land in the game's font
surface, but the frame is still uniform white: the game's NV4097 draws
(textured quads) are the stubbed s_backend and never rasterize. Next: the
software draw_arrays rasterizer (positions/UVs from the immediate vertex
decode + the tex0 surface) to composite the text onto the display buffer.

## Follow-up 23: software rasterizer hooked (DRAW_ARRAYS -> triangle fill)

The game's text uses NV4097_DRAW_ARRAYS (prim=5 triangle strip, 6 verts), which
the null backend dropped. Hooked rsx_software_draw_arrays on Linux: reads the
attrib-0 positions (float x/y, biased-format frac recovery like the D3D12
backend's dbgfont path) + the attrib-3 color (float RGBA) from the resolved
guest VAs, then barycentric triangle fill into the local-memory surface.
Observed draws: surf 0/0x3A0000, stride 52, tex0 = a 64x64 atlas (rect
0x400040, pitch 256). The vertices at draw time are still zeros/garbage (the
game uploads the UI vertex data on a later frame), so the frame stays white.
Next: capture the vertex uploads (SET_VERTEX_DATA timing) + sample the atlas
so the glyph quads composite.

## Follow-up 24: draw data observed -- the game's UI VAs are unpopulated at draw time

The rasterizer logs every draw (draw#1..N): the game's vertex buffers
(0x0200EC70, 0x020474C0, stride 52) hold x=0.5/y=0.5 + black, or x=0/y=0 +
garbage colors, and never change across the whole run -- the game's UI vertex
data is not uploaded to those addresses on this path (the game's real upload
must go through its own DMA/SPU or a different base offset). The draw pipeline
itself is complete and verified: DRAW_ARRAYS -> attrib-0 positions -> biased
frac recovery -> barycentric fill -> local-memory surface. Next: trace the
game's vertex upload (who writes 0x0200EC70) to get the real quad data.

## Follow-up 25: the game's UI text goes through its own surface, not the DRAW_ARRAYS

The DRAW_ARRAYS vertex buffers (0x0200EC70/0x020474C0) stay all-zero for the
whole run -- those draws are the game's placeholder clears, not the text. The
game's real text path: cellFontRenderCharGlyphImage renders the glyphs into
the game's surface buffer, then the game blits (NV0039 m2mf, already working)
to the UI surface 0xC0AE0700 and on to the display. The remaining stub is
cellFontRenderSurfaceInit (TJ stub returns 0 without initializing the game's
render surface descriptor), so the glyph blits copy from an unset buffer.
Next: implement cellFontRenderSurfaceInit (write the game's surface fields)
so the glyph -> surface -> blit -> display chain completes.

## Follow-up 26: the glyph surface descriptor is never initialized

RenderGlyph's diagnostic: the game's surface (0xD00FF818) has buf=0 and
garbage dims -- cellFontRenderSurfaceInit is still the TJ stub, so the game's
surface descriptor is never filled and the glyph blit has no buffer. The
game calls it with (surface, 0, 0x40, 1) and (renderer, 0xA8A8FA0, 0x1E00, 4)
-- two call shapes, so the real SDK signature needs to be matched. Next:
implement cellFontRenderSurfaceInit (fill buf/widthByte/pixelSizeByte/
width/height in the guest surface) for both call shapes.

## Follow-up 27: RenderSurfaceInit implemented; rasterizer produces visible pixels

Registered cellFontRenderSurfaceInit (0x90B9465E) in the NID table and
implemented it: (surface, buffer, bufferWidthByte, pixelSizeByte, width,
height) -- the game's two surfaces now decode correctly: a 64x64 A8 glyph
surface (buf=0) and a 1920x24 RGBA surface (buf=0xA8A8FA0). The software
rasterizer also started producing visible output (a black band at y=0-1 from
the game's still-garbage UI vertices). The glyph surface's buffer is NULL --
the game allocates it itself and writes the descriptor's buffer field later;
until then the glyph blit has no source. Next: watch who writes the 0xD00FF818
descriptor's buffer field (the game's own alloc), or route the glyph rendering
into the game's 1920x24 surface.
