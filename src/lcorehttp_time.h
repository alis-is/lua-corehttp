#ifndef LCOREHTTP_TIME_H
#define LCOREHTTP_TIME_H

#include <stdint.h>

#ifdef _WIN32
#include <sysinfoapi.h>
#else
#include <sys/time.h>
#endif

/**
 * @brief Get the current time in milliseconds.
 *
 * @return The current time in milliseconds.
 */
uint32_t
l_corehttp_get_time_ms(void) {
#ifdef _WIN32
    FILETIME ft;
    ULARGE_INTEGER uli;
    GetSystemTimeAsFileTime(&ft);
    uli.LowPart = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;
    return (uint32_t)((uli.QuadPart / 10000ULL) - (11644473600000ULL * 10000ULL));
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (tv.tv_sec * 1000) + (tv.tv_usec / 1000);
#endif
}

#endif /* LCOREHTTP_TIME_H */