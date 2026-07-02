#pragma once

using pthread_t = void*;

inline pthread_t pthread_self()
{
    return nullptr;
}

inline int pthread_setname_np(pthread_t, const char*)
{
    return 0;
}

inline int pthread_setname_np(const char*)
{
    return 0;
}

inline int pthread_getname_np(pthread_t, char* buffer, int size)
{
    if (buffer && size > 0)
        buffer[0] = 0;
    return 0;
}

