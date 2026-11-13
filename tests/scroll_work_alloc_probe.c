// Opt-in Linux/glibc UI-thread allocation probe for --scroll-profile.
// Build as a temporary LD_PRELOAD library; never link into the application.
#include <stdbool.h>
#include <stddef.h>
#include <features.h>

#if !defined(__GLIBC__)
#error This optional probe requires glibc.
#endif

extern void* __libc_malloc(size_t);
extern void* __libc_calloc(size_t, size_t);
extern void* __libc_realloc(void*, size_t);

static _Thread_local bool tracking;
static _Thread_local unsigned long long calls;

unsigned long long clashflux_scroll_allocations(bool start) {
    tracking = start;
    if (start) calls = 0;
    return calls;
}

void* malloc(size_t size) {
    if (tracking) ++calls;
    return __libc_malloc(size);
}

void* calloc(size_t count, size_t size) {
    if (tracking) ++calls;
    return __libc_calloc(count, size);
}

void* realloc(void* pointer, size_t size) {
    if (tracking) ++calls;
    return __libc_realloc(pointer, size);
}
