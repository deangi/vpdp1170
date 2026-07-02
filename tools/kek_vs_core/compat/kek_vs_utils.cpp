#include "utils.h"

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <io.h>

void setBit(uint16_t& v, const int bit, const bool vb)
{
    if (vb)
        v |= uint16_t(1u << bit);
    else
        v &= uint16_t(~(1u << bit));
}

int parity(int v)
{
    v ^= v >> 8;
    v ^= v >> 4;
    v &= 0x0f;
    return (0x6996 >> v) & 1;
}

std::string format(const char* const fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    va_list copy;
    va_copy(copy, ap);
    const int len = std::vsnprintf(nullptr, 0, fmt, copy);
    va_end(copy);
    if (len < 0) {
        va_end(ap);
        return {};
    }

    std::string out(size_t(len), '\0');
    std::vsnprintf(out.data(), out.size() + 1, fmt, ap);
    va_end(ap);
    return out;
}

std::string to_hex(const uint8_t* const data, const size_t n_bytes)
{
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(n_bytes * 2);
    for (size_t i = 0; i < n_bytes; ++i) {
        out.push_back(kHex[data[i] >> 4]);
        out.push_back(kHex[data[i] & 0x0f]);
    }
    return out;
}

std::vector<std::string> split(std::string in, const std::string& splitter)
{
    std::vector<std::string> out;
    if (splitter.empty()) {
        out.push_back(std::move(in));
        return out;
    }

    size_t pos = 0;
    for (;;) {
        const size_t next = in.find(splitter, pos);
        if (next == std::string::npos) {
            out.push_back(in.substr(pos));
            return out;
        }
        out.push_back(in.substr(pos, next - pos));
        pos = next + splitter.size();
    }
}

uint64_t get_us()
{
    using clock = std::chrono::steady_clock;
    return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                        clock::now().time_since_epoch())
                        .count());
}

uint64_t get_ms()
{
    return get_us() / 1000;
}

void myusleep(uint64_t us)
{
    std::this_thread::sleep_for(std::chrono::microseconds(us));
}

std::string get_thread_name()
{
    return "kek_vs_core";
}

void set_thread_name(std::string)
{
}

ssize_t WRITE(int fd, const char* whereto, size_t len)
{
    return _write(fd, whereto, unsigned(len));
}

ssize_t READ(int fd, char* whereto, size_t len)
{
    return _read(fd, whereto, unsigned(len));
}

void update_word(uint16_t* const w, const bool msb, const uint8_t v)
{
    if (msb)
        *w = uint16_t((*w & 0x00ff) | (uint16_t(v) << 8));
    else
        *w = uint16_t((*w & 0xff00) | v);
}

void set_nodelay(const int)
{
}

std::string get_endpoint_name(const int fd)
{
    return format("fd:%d", fd);
}

std::string get_configuration_string(const std::string&, const std::string& default_value)
{
    return default_value;
}

uint32_t get_configuration_uint32(const std::string&, const uint32_t default_value)
{
    return default_value;
}

bool put_configuration_uint32(const std::string&, const uint32_t)
{
    return false;
}

bool put_configuration_string(const std::string&, const std::string&)
{
    return false;
}

bool file_exists(const std::string& file)
{
    std::ifstream in(file, std::ios::binary);
    return in.good();
}
