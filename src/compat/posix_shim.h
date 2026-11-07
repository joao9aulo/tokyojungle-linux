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

/* Thread-local storage qualifier (matches the runtime's ppu_context.h). */
#ifndef PPU_THREAD_LOCAL
#  ifdef _MSC_VER
#    define PPU_THREAD_LOCAL __declspec(thread)
#  else
#    define PPU_THREAD_LOCAL __thread
#  endif
#endif

/* --- basic Win32 type aliases ------------------------------------------ */
typedef void*          HANDLE;
typedef void*          LPVOID;
typedef uint32_t       DWORD;
typedef uint32_t       ULONG;
typedef uint64_t       ULONGLONG;
typedef long long      LONGLONG;
typedef int32_t        LONG;
typedef int            BOOL;
typedef uintptr_t      UINT_PTR;
typedef size_t         SIZE_T;
/* Windows LARGE_INTEGER: a 64-bit value reachable as .QuadPart (and as
 * .LowPart/.HighPart, or the .low/.high the older shim used). */
typedef union {
    struct { DWORD LowPart; LONG HighPart; };
    struct { DWORD low; LONG high; };
    long long QuadPart;
} LARGE_INTEGER;

#ifndef FALSE
#define FALSE 0
#endif
#ifndef TRUE
#define TRUE  1
#endif
#define INFINITE 0xFFFFFFFFu
#define ERROR_TIMEOUT 1460u
#define WAIT_TIMEOUT  258u

/* x64 ignores __stdcall; make the WINAPI decoration a no-op. */
#define WINAPI
#define CALLBACK
#define STDMETHODCALLTYPE
#define __stdcall

#define DUPLICATE_SAME_ACCESS 0x00000002

/* --- module / stack-trace diagnostics (stubs) --------------------------- */
typedef HANDLE        HMODULE;
typedef const char*   LPCSTR;
#define GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS       0x00000004u
#define GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT 0x00000002u
static inline HMODULE GetModuleHandleA(LPCSTR name) { (void)name; return NULL; }
static inline BOOL GetModuleHandleExA(DWORD flags, LPCSTR name, HMODULE* mod)
{ (void)flags; (void)name; if (mod) *mod = NULL; return FALSE; }
static inline unsigned short RtlCaptureStackBackTrace(DWORD a, DWORD b,
                                                      void** c, DWORD* d)
{ (void)a; (void)b; (void)c; (void)d; return 0; }

/* --- critical section / event / semaphore / virtual-memory stubs ---------
 * The patched D3D12 renderer and a few runtime diagnostics use these; on a
 * non-Windows build the renderer is a no-op, so a correct-but-inert stub is
 * enough. */
typedef struct { pthread_mutex_t m; } CRITICAL_SECTION;
static inline void InitializeCriticalSection(CRITICAL_SECTION* c) { pthread_mutex_init(&c->m, NULL); }
static inline void EnterCriticalSection(CRITICAL_SECTION* c)       { pthread_mutex_lock(&c->m); }
static inline void LeaveCriticalSection(CRITICAL_SECTION* c)       { pthread_mutex_unlock(&c->m); }
static inline void DeleteCriticalSection(CRITICAL_SECTION* c)      { pthread_mutex_destroy(&c->m); }

#define WAIT_OBJECT_0 0u
#define WAIT_TIMEOUT  258u
static inline HANDLE CreateEventA(void* a, BOOL m, BOOL s, const char* n)
{ (void)a; (void)m; (void)s; (void)n; return (HANDLE)0x1; }
static inline HANDLE CreateEventW(void* a, BOOL m, BOOL s, const void* n)
{ (void)a; (void)m; (void)s; (void)n; return (HANDLE)0x1; }
static inline BOOL SetEvent(HANDLE h)      { (void)h; return TRUE; }
static inline BOOL ResetEvent(HANDLE h)    { (void)h; return TRUE; }
static inline DWORD WaitForSingleObject(HANDLE h, DWORD ms)
{ (void)h; (void)ms; return WAIT_OBJECT_0; }
static inline DWORD WaitForMultipleObjects(DWORD n, const HANDLE* h, BOOL all, DWORD ms)
{ (void)n; (void)h; (void)all; (void)ms; return WAIT_OBJECT_0; }
static inline HANDLE CreateSemaphoreA(void* a, LONG i, LONG m, const char* n)
{ (void)a; (void)i; (void)m; (void)n; return (HANDLE)0x1; }
static inline HANDLE CreateSemaphoreW(void* a, LONG i, LONG m, const void* n)
{ (void)a; (void)i; (void)m; (void)n; return (HANDLE)0x1; }
static inline BOOL ReleaseSemaphore(HANDLE h, LONG r, LONG* prev)
{ (void)h; (void)r; if (prev) *prev = 0; return TRUE; }

static inline void* VirtualAlloc(void* addr, size_t sz, DWORD type, DWORD prot)
{ (void)addr; (void)type; (void)prot; return malloc(sz ? sz : 1); }
static inline BOOL VirtualFree(void* addr, size_t sz, DWORD type)
{ (void)sz; (void)type; if (addr) free(addr); return TRUE; }
static inline void* LoadLibraryW(const void* n) { (void)n; return NULL; }
static inline void* GetProcAddress(void* m, const char* n) { (void)m; (void)n; return NULL; }
static inline BOOL FreeLibrary(void* m) { (void)m; return FALSE; }
static inline DWORD GetModuleFileNameA(void* m, char* out, DWORD sz)
{ (void)m; if (out && sz) out[0] = 0; return 0; }
static inline DWORD GetModuleFileNameW(void* m, void* out, DWORD sz)
{ (void)m; (void)out; (void)sz; return 0; }
static inline HANDLE CreateFileA(const char* n, DWORD a, DWORD s, void* sd,
                                 DWORD c, DWORD f, void* t)
{ (void)n; (void)a; (void)s; (void)sd; (void)c; (void)f; (void)t; return NULL; }
static inline HANDLE CreateFileW(const void* n, DWORD a, DWORD s, void* sd,
                                 DWORD c, DWORD f, void* t)
{ (void)n; (void)a; (void)s; (void)sd; (void)c; (void)f; (void)t; return NULL; }
static inline BOOL ReadFile(HANDLE h, void* b, DWORD n, DWORD* r, void* o)
{ (void)h; (void)b; (void)n; (void)o; if (r) *r = 0; return FALSE; }
static inline BOOL WriteFile(HANDLE h, const void* b, DWORD n, DWORD* r, void* o)
{ (void)h; (void)b; (void)n; (void)o; if (r) *r = 0; return FALSE; }

/* --- monotonic clock ---------------------------------------------------- */
static inline ULONGLONG GetTickCount64(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (ULONGLONG)ts.tv_sec * 1000ull + (ULONGLONG)(ts.tv_nsec / 1000000);
}

/* --- high-resolution timer (QueryPerformanceCounter) -------------------- */
static inline BOOL QueryPerformanceFrequency(LARGE_INTEGER* f)
{
    if (f) f->QuadPart = 1000000000LL;   /* 1 GHz == nanoseconds */
    return TRUE;
}
static inline BOOL QueryPerformanceCounter(LARGE_INTEGER* c)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    if (c) c->QuadPart = (long long)ts.tv_sec * 1000000000LL + (long long)ts.tv_nsec;
    return TRUE;
}

/* --- slim reader/writer lock + condition variable -----------------------
 * Windows SRWLOCK/CONDITION_VARIABLE are used by the patched cellGcmSys.c.
 * Map them to a plain pthread mutex (shared acquires take the exclusive lock:
 * correct, just not lock-free) and a pthread condition variable. */
typedef struct { pthread_mutex_t m; } SRWLOCK;
typedef struct { pthread_cond_t  c; } CONDITION_VARIABLE;

#define SRWLOCK_INIT            ((SRWLOCK){ .m = PTHREAD_MUTEX_INITIALIZER })
#define CONDITION_VARIABLE_INIT ((CONDITION_VARIABLE){ .c = PTHREAD_COND_INITIALIZER })

#define CONDITION_VARIABLE_LOCKMODE_SHARED 0x1

static inline void InitializeSRWLock(SRWLOCK* l)  { pthread_mutex_init(&l->m, NULL); }
static inline void AcquireSRWLockExclusive(SRWLOCK* l) { pthread_mutex_lock(&l->m); }
static inline void ReleaseSRWLockExclusive(SRWLOCK* l) { pthread_mutex_unlock(&l->m); }
static inline void AcquireSRWLockShared(SRWLOCK* l)    { pthread_mutex_lock(&l->m); }
static inline void ReleaseSRWLockShared(SRWLOCK* l)    { pthread_mutex_unlock(&l->m); }
static inline BOOL TryAcquireSRWLockExclusive(SRWLOCK* l)
{ return pthread_mutex_trylock(&l->m) == 0; }
static inline BOOL TryAcquireSRWLockShared(SRWLOCK* l)
{ return pthread_mutex_trylock(&l->m) == 0; }

static inline void InitializeConditionVariable(CONDITION_VARIABLE* cv)
{ pthread_cond_init(&cv->c, NULL); }
static inline void WakeConditionVariable(CONDITION_VARIABLE* cv)
{ pthread_cond_signal(&cv->c); }
static inline void WakeAllConditionVariable(CONDITION_VARIABLE* cv)
{ pthread_cond_broadcast(&cv->c); }
static inline BOOL SleepConditionVariableSRW(CONDITION_VARIABLE* cv, SRWLOCK* l,
                                             DWORD ms, ULONG flags)
{
    (void)flags;   /* the mutex mapping makes shared/exclusive equivalent */
    if (ms == INFINITE) {
        return pthread_cond_wait(&cv->c, &l->m) == 0;
    } else {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec  += ms / 1000;
        ts.tv_nsec += (long)(ms % 1000) * 1000000L;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        return pthread_cond_timedwait(&cv->c, &l->m, &ts) == 0;
    }
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
#ifndef _WIN32
static inline long _InterlockedExchange(long volatile* p, long v)
{
    return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST);
}
static inline long _InterlockedExchangeAdd(long volatile* p, long v)
{
    return __atomic_add_fetch(p, v, __ATOMIC_SEQ_CST);
}
static inline void YieldProcessor(void) { __asm__ volatile("pause"); }
#endif

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
