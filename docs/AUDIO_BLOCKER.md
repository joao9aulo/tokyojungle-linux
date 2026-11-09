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
