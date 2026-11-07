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
