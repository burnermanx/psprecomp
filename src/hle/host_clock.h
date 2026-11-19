/* Wall-clock microseconds for trace stamps. timespec_get/TIME_UTC is C11 and
 * not every MinGW-w64 header set declares it, so use the OS call directly. */
#ifndef PSPRECOMP_HOST_CLOCK_H
#define PSPRECOMP_HOST_CLOCK_H

#include <stdint.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
static uint64_t host_clock_us(void) {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);   /* 100 ns ticks since 1601-01-01 */
    uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return t / 10u - 11644473600ull * 1000000u;   /* to the Unix epoch */
}
#else
#include <time.h>
static uint64_t host_clock_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}
#endif

#endif
