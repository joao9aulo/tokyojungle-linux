/* POSIX (Linux) shims for the Win32 APIs used by the Tokyo Jungle port.
 *
 * The port was written for Windows (threads, timing, a vectored exception
 * handler for on-demand guest-page commit, and SEH crash reporting). This
 * header provides enough of a Win32 surface on non-Windows hosts that the
 * same translation units compile and run against pthread / clock_gettime /
 * sigaction instead. It is NOT a general Win32 emulation layer -- only the
 * subset the port actually calls is here.
 */
#ifndef TJ_POSIX_SHIM_H
#define TJ_POSIX_SHIM_H

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>
#include <sys/syscall.h>

/* --- basic Win32 type aliases ------------------------------------------ */
typedef void*          HANDLE;
typedef void*          LPVOID;
typedef uint32_t       DWORD;
typedef uint64_t       ULONGLONG;
typedef int32_t        LONG;
typedef int            BOOL;
typedef uintptr_t      UINT_PTR;
typedef size_t         SIZE_T;
typedef struct { DWORD low; LONG high; } LARGE_INTEGER;

#ifndef FALSE
#define FALSE 0
#endif
#ifndef TRUE
#define TRUE  1
#endif
#define INFINITE 0xFFFFFFFFu

/* x64 ignores __stdcall; make the WINAPI decoration a no-op. */
#define WINAPI
#define CALLBACK
#define STDMETHODCALLTYPE

#define DUPLICATE_SAME_ACCESS 0x00000002

/* --- monotonic clock ---------------------------------------------------- */
static inline ULONGLONG GetTickCount64(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (ULONGLONG)ts.tv_sec * 1000ull + (ULONGLONG)(ts.tv_nsec / 1000000);
}

/* --- sleep -------------------------------------------------------------- */
static inline void Sleep(DWORD ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

/* --- thread id / error -------------------------------------------------- */
static inline DWORD GetCurrentThreadId(void)
{
    return (DWORD)syscall(SYS_gettid);
}
static inline DWORD GetLastError(void) { return (DWORD)errno; }

/* --- atomics ------------------------------------------------------------ */
static inline LONG InterlockedIncrement(LONG volatile* p)
{
    return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST);
}

/* --- thread creation ------------------------------------------------------
 * The port passes C++ lambdas to CreateThread (MSVC allows a raw function
 * pointer in a lambda with no captures). Those call sites are guarded with
 * #ifdef _WIN32 and replaced by std::thread / pthread on POSIX, so this
 * trampoline only needs to cover the plain DWORD WINAPI (LPVOID) cases in
 * tj_present.cpp / dispatch_glue.cpp. */
typedef DWORD (WINAPI *LPTHREAD_START_ROUTINE)(LPVOID);

struct tj_pthread_wrap {
    LPTHREAD_START_ROUTINE fn;
    LPVOID arg;
};
static inline void* tj_pthread_tramp(void* p)
{
    struct tj_pthread_wrap* w = (struct tj_pthread_wrap*)p;
    LPTHREAD_START_ROUTINE fn = w->fn;
    LPVOID arg = w->arg;
    free(w);
    return (void*)(uintptr_t)fn(arg);
}

static inline HANDLE CreateThread(LPVOID attr, SIZE_T stack,
                                  LPTHREAD_START_ROUTINE fn, LPVOID arg,
                                  DWORD flags, DWORD* tid)
{
    (void)attr; (void)stack; (void)flags; (void)tid;
    struct tj_pthread_wrap* w = (struct tj_pthread_wrap*)malloc(sizeof *w);
    if (!w) return NULL;
    w->fn = fn; w->arg = arg;
    pthread_t* th = (pthread_t*)malloc(sizeof(pthread_t));
    if (!th) { free(w); return NULL; }
    if (pthread_create(th, NULL, tj_pthread_tramp, w) != 0) {
        free(th); free(w); return NULL;
    }
    return (HANDLE)th;
}

static inline BOOL CloseHandle(HANDLE h)
{
    if (!h) return FALSE;
    /* We only ever CreateThread() + CloseHandle() the watchdog handle. */
    free(h);
    return TRUE;
}

/* --- thread context introspection (watchdog) -----------------------------
 * Reading another thread's host registers on Linux needs ptrace(), which is
 * far too heavy for a diagnostic watchdog. Return an empty context so the
 * watchdog still samples the guest ppu_context (LR/CTR/SP come from there).
 */
typedef struct {
    DWORD ContextFlags;
    uintptr_t Rip;
} CONTEXT;
#define CONTEXT_CONTROL  0x00100001L
#define CONTEXT_INTEGER  0x00100002L

static inline HANDLE GetCurrentProcess(void) { return (HANDLE)1; }
static inline HANDLE GetCurrentThread(void) { return (HANDLE)1; }
static inline BOOL DuplicateHandle(HANDLE a, HANDLE b, HANDLE c, HANDLE* out,
                                   DWORD acc, BOOL inherit, DWORD opts)
{ (void)a; (void)b; (void)c; (void)acc; (void)inherit; (void)opts;
  *out = (HANDLE)1; return TRUE; }
static inline DWORD SuspendThread(HANDLE h) { (void)h; return (DWORD)-1; }
static inline DWORD ResumeThread(HANDLE h)  { (void)h; return (DWORD)-1; }
static inline BOOL GetThreadContext(HANDLE h, CONTEXT* c)
{ (void)h; if (c) { c->Rip = 0; c->ContextFlags = 0; } return TRUE; }

#endif /* TJ_POSIX_SHIM_H */
