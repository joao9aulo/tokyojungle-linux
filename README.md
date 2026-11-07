# Tokyo Jungle Recompiled — Linux Port

> **A Pomeranian walks into post-apocalyptic Tokyo. On Linux.**

A native PC port of **Tokyo Jungle (2012)** for Linux, forked from
**[SilvioDiaz/tokyojungle](https://github.com/SilvioDiaz/tokyojungle)**
("Tokyo Jungle Recompiled"), which is in turn built on
**[sp00nznet/ps3recomp](https://github.com/sp00nznet/ps3recomp)** — a static
recompilation toolchain that translates PS3 PowerPC binaries into native C/C++.

No emulator. No compatibility layers. Just recompiled native code.

---

## About this fork

The upstream project targets **Windows (D3D12)** and the **NPUA80523 (NA
digital)** build of the game. This fork adds:

| | Upstream | This fork |
|---|---|---|
| Host OS | Windows (D3D12) | **Linux** (GCC) |
| Game build | NPUA80523 (NA) | **BCAS20219 (Japan disc)** |
| Render backend | D3D12 | headless (null) — Vulkan is the next step |
| Heap redirect | NA addresses | re-derived for the JP binary |

The port's hard-coded guest addresses (heap wrappers, CRT overrides, the TOC)
differ between the NA and JP builds, so they were re-derived against the
Japan disc. See [docs/LINUX_PORT.md](docs/LINUX_PORT.md) for the details.

## Status

**Boots to the game's main loop, headless.**

```
[ELF] Function descriptor (OPD) at 0x003428F0 → Code entry: 0x00010230
[HLE] 1238 handlers | 338 OPDs created | 7916 functions lifted
[HLE] Heap initialized (192 MB)
[HLE] cellSysmoduleLoadModule / _cellGcmInitBody  ✓
→ game main loop (cellPad + GCM polling)          ✓
```

The recompiled game runs its boot chain end-to-end (CRT init, module loading,
GCM init) and enters its frame loop. What is **not** there yet: a Vulkan
render backend, the game-clock tick (the counter the main loop waits on), and
a few GCM imports. See [docs/LINUX_PORT.md](docs/LINUX_PORT.md) for the
current blocker.

## Building

### Prerequisites

- **Linux** (x86-64), **GCC 12+** (C++20), **CMake 3.20+**, **Ninja**
- **Python 3.10+** with `pycryptodome`, `capstone`, `construct`, `tabulate`, `tqdm`
- **SDL2** development files (`libsdl2-dev`)
- **ps3recomp** — see below
- **A legitimate copy of Tokyo Jungle** (BCAS20219 / JP disc) — you must own it

### Steps

```bash
# 1. Clone this repo
git clone --recursive https://github.com/<you>/tokyojungle-linux.git
cd tokyojungle-linux

# 2. Set up ps3recomp next to it (the CMake build looks for ../ps3recomp)
git clone https://github.com/sp00nznet/ps3recomp.git ../ps3recomp

# 3. Python dependencies
pip install -r requirements.txt

# 4. Provide your decrypted ELF (you must do this yourself)
#    Place EBOOT.BIN (from your JP disc) in input/ and decrypt:
python scripts/decrypt_self.py input/EBOOT.BIN
#    → input/EBOOT.ELF

# 5. Analysis + lifting (generates generated/ppu_recomp_*.cpp)
python ../ps3recomp/tools/find_functions.py input/EBOOT.ELF \
    --output generated/functions.json
python ../ps3recomp/tools/ppu_lifter.py input/EBOOT.ELF \
    --functions generated/functions.json \
    --output generated \
    --header-name ppu_recomp.h \
    --source-name ppu_recomp.c \
    --jobs 2 --chunk-lines 150000 --code-end 0x260000
python scripts/post_lift.py   # heap redirects

# 6. Build (the runtime also needs the HLE NID registration unit)
python ../ps3recomp/tools/gen_hle_nids.py --all \
    --out src/gen/ppu_hle_nids.cpp

cmake -B build-linux -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_FLAGS_RELEASE="-O1 -DNDEBUG" \
    -DCMAKE_CXX_FLAGS_RELEASE="-O1 -DNDEBUG"
cmake --build build-linux -j2

# 7. Run (headless)
./build-linux/tokyojungle
```

> **Note:** `input/` and `generated/` are gitignored. This repository contains
> no game data and no generated code. The `-O1` flags keep the ~150k-line
> generated translation units within an 8 GB RAM budget; use `-O3` on larger
> machines.

## Game IDs

| Region | ID | Format | Status here |
|---|---|---|---|
| JP (Disc) | **BCAS20219** | Blu-ray | **supported** |
| NA (Digital) | NPUA80523 | PSN | upstream (untested here) |
| EU (Digital) | NPEA00421 | PSN | untested |

## License

The code in this repository is MIT licensed — see [LICENSE](LICENSE). That
covers *our* code only: the port scaffold, HLE glue, build scripts, and the
Linux/JP adaptations. It does not and cannot cover Tokyo Jungle itself, and
no game data is included here.

## Legal

This is a clean-room recompilation toolchain output. It does not contain any
proprietary Sony or game code. You must provide your own legally obtained game
files. This project exists solely for game preservation purposes.

**Tokyo Jungle** is a trademark of Sony Interactive Entertainment. This project
is not affiliated with, endorsed by, or connected to Sony, Japan Studio, or
Crispy's in any way.

---

*Forked from [SilvioDiaz/tokyojungle](https://github.com/SilvioDiaz/tokyojungle) ·
Built with [ps3recomp](https://github.com/sp00nznet/ps3recomp) ·
Let's keep Tokyo Jungle alive.*
