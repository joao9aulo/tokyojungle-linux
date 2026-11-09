/* Present / vblank ticker.
 *
 * Nothing in Tokyo Jungle's harness was advancing the display. The game
 * requests a flip (_cellGcmSetFlipCommand), then polls cellGcmGetFlipStatus
 * until it clears -- and it never did, because cellGcmTickFlip is what clears
 * it and no thread was calling it. The boot sat in that poll forever.
 *
 * ps3recomp's own harness (runtime/ppu/tests/boot_main.cpp) and the LBP port
 * both run a thread like this; TJ has its own main() and so needs its own.
 *
 * The ticks are driven off real elapsed time rather than off how long
 * present() takes: on a hidden or occluded window DXGI throttles Present
 * hard, and pacing the guest's frame counter behind it drops the whole boot
 * to a few frames a second.
 *
 * ponytail: a straight copy of the harness cadence (16 ms ticks, drain at the
 * outer 4 ms cadence). If TJ turns out to need different pacing, measure
 * before changing it -- the drain interval is load-bearing for titles that
 * fence every render pass.
 */
#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#include <windows.h>
#else
#include "compat/posix_shim.h"
#endif

#include "ppu_context.h"
#include "recomp_bridge.h"   /* vm_write64 (diagnostic clock tick) */
#include "sys_ppu_thread.h"

/* TJ_THREADS=1: every 2 s, print each PPU thread's state and the guest address
 * of its last syscall/HLE call (prof_pc). The watchdog only samples the main
 * thread, so a worker that finished early or wedged is otherwise invisible --
 * and "the game runs its frame loop but never loads anything" is exactly the
 * shape a wedged worker produces. */
static void tj_dump_threads(void)
{
    static int on = -1;
    if (on < 0) on = getenv("TJ_THREADS") ? 1 : 0;
    if (!on) return;

    static ULONGLONG next = 0;
    ULONGLONG now = GetTickCount64();
    if (now < next) return;
    next = now + 2000;

    static const char* kState[] = { "FREE", "RUNNING", "FINISHED", "DETACHED" };
    fprintf(stderr, "[threads]\n");
    for (int i = 0; i < PPU_THREAD_MAX; i++) {
        const ppu_thread_info* t = &g_ppu_threads[i];
        if (t->state == PPU_THREAD_STATE_FREE) continue;
        fprintf(stderr, "   tid=%d %-9s entry=0x%08llX prof_pc=0x%08X name=\"%s\"\n",
                i, kState[t->state & 3],
                (unsigned long long)t->entry_addr, t->prof_pc, t->name);
    }
}

extern "C" {
int  rsx_d3d12_backend_init(int width, int height, const char* title);
void rsx_d3d12_backend_present(void);
int  rsx_d3d12_backend_pump_messages(void);
#define TJ_RSX_INIT    rsx_d3d12_backend_init
#define TJ_RSX_PRESENT rsx_d3d12_backend_present
#define TJ_RSX_PUMP    rsx_d3d12_backend_pump_messages
void cellGcmTickVBlank(void);
void cellGcmTickFlip(void);
void cellGcm_rsx_process_fifo(void);
int  cellGcm_take_flip_pending(void);
}

static DWORD WINAPI tj_present_thread(LPVOID)
{
    const char* title = getenv("PS3_TITLE");
    if (!title || !*title) title = "Tokyo Jungle (ps3recomp)";

    int rsx_ok = (TJ_RSX_INIT(1280, 720, title) == 0);
    fprintf(stderr, "[rsx] backend init %s\n",
            rsx_ok ? "OK -- window open" : "FAILED");

    ULONGLONG next_tick = GetTickCount64();
    for (;;) {
        Sleep(4);
        ULONGLONG now = GetTickCount64();

        int fired = 0;
        while ((long long)(now - next_tick) >= 0 && fired < 240) {
            /* TJ_TICK_GAMECLOCK=<guestEA>: DIAGNOSTIC ONLY.
             *
             * The main loop waits for [[TOC+0x1860]+0x18] to pass a stored
             * timestamp + 3, and that field reads 0 forever -- nothing writes
             * it: not guest code, not an HLE, not SPU DMA (all three checked).
             * Ticking it by hand says whether the wait is really what holds the
             * boot, and in what units, without guessing.
             *
             * This is not a fix. Whatever should own that clock still has to be
             * found; poking it from outside just makes the guest believe time
             * passed. */
            { static long ea = -1;
              if (ea < 0) { const char* e = getenv("TJ_TICK_GAMECLOCK");
                            ea = e ? strtol(e, nullptr, 0) : 0; }
              if (ea > 0) { static uint64_t t = 0; vm_write64((uint32_t)ea, ++t); } }

            cellGcmTickVBlank();
            cellGcmTickFlip();
            if (rsx_ok && cellGcm_take_flip_pending())
                TJ_RSX_PRESENT();
            cellGcm_rsx_process_fifo();
            /* TJ_FBDUMP=/path: every ~2 s dump the guest display buffer
             * (local mem 0xC0000000 + id0, 1280x720, pitch 5120) as raw BGRA
             * so a host script can render the frame. */
            { static int _fb = -1; static ULONGLONG _t0 = 0; static unsigned _n = 0;
              if (_fb < 0) { const char* e = getenv("TJ_FBDUMP"); _fb = e && *e ? 1 : 0; _t0 = GetTickCount64(); }
              if (_fb && GetTickCount64() - _t0 >= 2000) {
                  _t0 = GetTickCount64();
                  extern uint8_t* vm_base;
                  if (vm_base) {
                      /* TJ_DEBUG_SCREEN: composite the game's real content into
                       * the display buffer so the boot is visible: the font
                       * atlas magnified 8x + the game's UI surface strip. */
                      if (getenv("TJ_DEBUG_SCREEN")) {
                          uint32_t* db = (uint32_t*)(vm_base + 0xC0000000ull);
                          for (int y = 0; y < 720; y++)
                              for (int x = 0; x < 1280; x++) db[y*1280+x] = 0x00FFFFFFu;
                          for (int ay = 0; ay < 64; ay++)
                              for (int ax = 0; ax < 64; ax++) {
                                  uint32_t p = *(uint32_t*)(vm_base + 0xCA369180ull + (size_t)ay*256 + (size_t)ax*4);
                                  uint32_t c = (p & 0x00FFFFFFu) | 0xFF000000u;
                                  if (c == 0xFF000000u) c = 0xFF404040u;
                                  for (int dy = 0; dy < 8; dy++)
                                      for (int dx = 0; dx < 8; dx++)
                                          db[(64 + ay*8 + dy)*1280 + (96 + ax*8 + dx)] = c;
                              }
                          for (int uy = 0; uy < 32; uy++)
                              for (int ux = 0; ux < 1024; ux++) {
                                  uint32_t p = *(uint32_t*)(vm_base + 0xC0AE0700ull + (size_t)uy*4096 + (size_t)ux*4);
                                  db[(600 + uy)*1280 + (96 + ux)] = p;
                              }
                      }
                      char path[512];
                      snprintf(path, sizeof(path), "%s/frame_%u.raw", getenv("TJ_FBDUMP"), _n++);
                      FILE* f = fopen(path, "wb");
                      if (f) {
                          for (unsigned y = 0; y < 720; y++)
                              fwrite(vm_base + 0xC0000000ull + (size_t)y * 5120, 1, 1280 * 4, f);
                          fclose(f);
                          fprintf(stderr, "[FBDUMP] wrote %s\n", path);
                      }
                      /* Also dump the game's UI surface (0xC0AE0700, 1024x32). */
                      snprintf(path, sizeof(path), "%s/ui.raw", getenv("TJ_FBDUMP"));
                      f = fopen(path, "wb");
                      if (f) {
                          for (unsigned y = 0; y < 32; y++)
                              fwrite(vm_base + 0xC0AE0700ull + (size_t)y * 4096, 1, 1024 * 4, f);
                          fclose(f);
                      }
                      /* Also dump the VA buffers (0x020474C0, 6 verts x 52). */
                      snprintf(path, sizeof(path), "%s/va2.raw", getenv("TJ_FBDUMP"));
                      f = fopen(path, "wb");
                      if (f) {
                          fwrite(vm_base + 0x020474C0u, 1, 6 * 52, f);
                          fclose(f);
                      }
                      /* Also dump the VA-as-VRAM (0xC0474C0, 6 verts x 52). */
                      snprintf(path, sizeof(path), "%s/va3.raw", getenv("TJ_FBDUMP"));
                      f = fopen(path, "wb");
                      if (f) {
                          fwrite(vm_base + 0xC0474C0u, 1, 6 * 52, f);
                          fclose(f);
                      }
                      /* Also dump the VA region (0x0204A00, 0x600). */
                      snprintf(path, sizeof(path), "%s/va4.raw", getenv("TJ_FBDUMP"));
                      f = fopen(path, "wb");
                      if (f) {
                          fwrite(vm_base + 0x0204A00u, 1, 0x600, f);
                          fclose(f);
                      }
                      /* Also dump the draw VA (0x0200F288, 6 verts x 52). */
                      snprintf(path, sizeof(path), "%s/va5.raw", getenv("TJ_FBDUMP"));
                      f = fopen(path, "wb");
                      if (f) {
                          fwrite(vm_base + 0x0200F288u, 1, 6 * 52, f);
                          fclose(f);
                      }
                      /* Also dump the draw VA raw (0x020474A0, 0x200). */
                      snprintf(path, sizeof(path), "%s/va6.raw", getenv("TJ_FBDUMP"));
                      f = fopen(path, "wb");
                      if (f) {
                          fwrite(vm_base + 0x020474A0u, 1, 0x200, f);
                          fclose(f);
                      }
                      /* Also dump the text line (0xA8A8FA0, 1920x24 RGBA). */
                      snprintf(path, sizeof(path), "%s/textline.raw", getenv("TJ_FBDUMP"));
                      f = fopen(path, "wb");
                      if (f) {
                          fwrite(vm_base + 0xA8A8FA0u, 1, 1920 * 24 * 4, f);
                          fclose(f);
                      }
                      /* Also dump the atlas (0xCA369180, 64x64). */
                      snprintf(path, sizeof(path), "%s/atlas.raw", getenv("TJ_FBDUMP"));
                      f = fopen(path, "wb");
                      if (f) {
                          for (unsigned y = 0; y < 64; y++)
                              fwrite(vm_base + 0xCA369180ull + (size_t)y * 256, 1, 64 * 4, f);
                          fclose(f);
                      }
                      /* Also dump the glyph scratch (0xC2000000, 64x64). */
                      snprintf(path, sizeof(path), "%s/glyphs.raw", getenv("TJ_FBDUMP"));
                      f = fopen(path, "wb");
                      if (f) {
                          fwrite(vm_base + 0xC2000000u, 1, 64 * 64, f);
                          fclose(f);
                      }
                      /* Also dump the game's vertex buffers for inspection. */
                      snprintf(path, sizeof(path), "%s/vbuf.raw", getenv("TJ_FBDUMP"));
                      f = fopen(path, "wb");
                      if (f) {
                          fwrite(vm_base + 0x0200EC70u, 1, 64 * 52, f);
                          fwrite(vm_base + 0x020474C0u, 1, 64 * 52, f);
                          fclose(f);
                          fprintf(stderr, "[FBDUMP] wrote vbuf.raw\n");
                      }
                  }
              } }
            next_tick += 16;          /* ~60 Hz */
            fired++;
        }
        if (fired >= 240)
            next_tick = now;          /* fell far behind -- resync */

        /* Also drain at the outer cadence: titles fence every render pass on
         * an RSX label the drain writes, and waiting a full 16 ms per fence
         * paces the guest to single-digit fps. */
        tj_dump_threads();

        if (rsx_ok) {
            if (cellGcm_take_flip_pending())
                TJ_RSX_PRESENT();
            if (TJ_RSX_PUMP() != 0)
                rsx_ok = 0;           /* window closed */
        }
        cellGcm_rsx_process_fifo();
    }
}

extern "C" void tj_start_present_thread(void)
{
    if (getenv("TJ_NO_PRESENT")) {
        printf("[TJ] TJ_NO_PRESENT: no present thread (headless)\n");
        return;
    }
    CreateThread(NULL, 4u * 1024 * 1024, tj_present_thread, NULL, 0, NULL);
    printf("[TJ] present thread started (60 Hz vblank/flip ticker)\n");
}
