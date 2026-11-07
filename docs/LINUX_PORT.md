# Linux port — technical notes (BCAS20219 / JP)

This document records the region-specific and Linux-specific work that makes
this fork boot the **Japan disc (BCAS20219)** on Linux, and the current state
of the bring-up. It is a working log; the README has the short version.

## 1. Region mismatch (the big one)

Upstream (`SilvioDiaz/tokyojungle`) hard-codes guest addresses for the
**NPUA80523 (NA digital)** build. The JP disc is a different binary:

| | NA (upstream) | JP (this fork) |
|---|---|---|
| TOC (`r2`) | `0x00359220` | **`0x003591E0`** |
| functions | 7,924 | 7,898 |
| `.opd` entries | 7,410 | 7,402 |
| `"mspace_free"` | — | `0x28CBA8` |
| `"bad allocation"` | — | `0x29FB80` / `0x29FB90` |

Every hard-coded guest address in `scripts/post_lift.py`, `scripts/gen_stubs.py`,
`src/hle_imports.h` and `src/import_resolver.h` is NA-specific and had to be
re-derived for JP.

### Heap wrappers (re-derived)

The guest allocator wrappers live at `0x0024Cxxx` and each loads the mspace
pointer from `TOC+0x29E8`, then calls a `mspace_*` core:

| function | JP address | redirects to |
|---|---|---|
| `malloc` | `0x0024C360` | `tj_heap_malloc` |
| `free` | `0x0024C25C` | `tj_heap_free` |
| `realloc` | `0x0024C28C` | `tj_heap_realloc` |
| `memalign` | `0x0024C2C4` | `tj_heap_memalign` |

These were found by tracing `operator new` (`func_0023B8FC`, which throws
"bad allocation") → `malloc`. Without them the guest `dlmalloc` mspace is never
initialised, every allocation returns 0, and the boot dies with
`exception: bad allocation`.

## 2. Linux porting (from the Windows/D3D12 upstream)

Upstream builds only on Windows (D3D12, SEH, XInput, WASAPI). The Linux work:

- **`src/compat/posix_shim.h`** (new): Win32 surface used by the port
  (`CreateThread`→pthread, `Sleep`, `GetTickCount64`, `HANDLE`/`DWORD`,
  thread-context stubs) for non-Windows hosts.
- **`src/main.cpp`**: `AddVectoredExceptionHandler`→`sigaction` (SIGSEGV
  fault-commit via `mprotect`), `VirtualProtect`→`mprotect`, the SEH
  `__try/__except` block already had a `#else` path.
- **`src/tj_present.cpp` / `src/dispatch_glue.cpp` / `src/tj_crt_overrides.cpp`**:
  conditional `<windows.h>` includes, `PPU_THREAD_LOCAL`, `backtrace()` instead
  of `RtlCaptureStackBackTrace`.
- **`CMakeLists.txt`**: `src/compat` is added only on WIN32 (it must not shadow
  `<unistd.h>`/`<dirent.h>` on POSIX).
- **`src/gen/ppu_hle_nids.cpp`** regenerated with the current ps3recomp
  `gen_hle_nids.py` (the checked-in one referenced `cellFiberPpu*` names that
  no longer exist).

## 3. Current state / next blocker

After the heap fix the boot reaches the game's main loop:

```
[HLE] Heap initialized (192 MB)
[heap] malloc(...) → 0x02xxxxxx
[HLE] cellSysmoduleLoadModule(0x001F)
[HLE] _cellGcmInitBody(...)
[import] UNIMPLEMENTED ? (slot 0x00342588)
→ spin loop: cellPad (input) + GCM (graphics) polling
```

The spin loop is the game's frame loop (`func_001F6140`, `func_001633E0`,
`func_001F19A8`) polling input and GCM. It busy-waits because:

1. **No render backend** (headless null backend; upstream used D3D12, this
   fork has no Vulkan backend yet), so flips/vblank never complete for real.
2. **The game clock** — upstream documented a counter at
   `[[TOC+0x1860]+0x18]` that never advances (NA address; the JP offset needs
   re-deriving). That is what the main loop waits on.
3. **`slot 0x342588`** is an unmapped GCM import (between `cellGcmSetTile`
   `0x34257C` and `cellGcmUnbindTile` `0x34258C`).

Next steps: re-derive the JP game-clock offset and tick it; map the missing
GCM import; then bring up the Vulkan RSX backend (ps3recomp has an opt-in
`PS3RECOMP_RSX_VULKAN`).

## Environment notes

- The build was exercised on Ubuntu 24.04, GCC 13.3, 8 GB RAM, using `-O1`
  (`-O3` needs ~4 GB per generated translation unit).
- The decryption key added to `scripts/decrypt_self.py` is the publicly
  documented PS3 `KEY_APP` revision `0x19` (from RPCS3's `key_vault.cpp`).
