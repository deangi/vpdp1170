#pragma once

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <thread>
#include <BaseTsd.h>

using ssize_t = SSIZE_T;
using namespace std::chrono_literals;

#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif

#ifndef TIMER_ABSTIME
#define TIMER_ABSTIME 1
#endif

inline int clock_gettime(int, timespec* ts)
{
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    const auto sec = std::chrono::duration_cast<std::chrono::seconds>(now);
    const auto nsec = std::chrono::duration_cast<std::chrono::nanoseconds>(now - sec);
    ts->tv_sec = static_cast<time_t>(sec.count());
    ts->tv_nsec = static_cast<long>(nsec.count());
    return 0;
}

inline int clock_nanosleep(int, int flags, const timespec* request, timespec*)
{
    if (flags == TIMER_ABSTIME) {
        const auto target =
            std::chrono::steady_clock::time_point(std::chrono::seconds(request->tv_sec) +
                                                  std::chrono::nanoseconds(request->tv_nsec));
        std::this_thread::sleep_until(target);
    }
    else {
        std::this_thread::sleep_for(std::chrono::seconds(request->tv_sec) +
                                    std::chrono::nanoseconds(request->tv_nsec));
    }
    return 0;
}
