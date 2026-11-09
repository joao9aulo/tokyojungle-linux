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
