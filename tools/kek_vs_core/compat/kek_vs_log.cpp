#include "log.h"

#include <cstdarg>
#include <cstdio>

log_ss_type log_mask_match = 0;

void setlogfile(const char* const, const bool)
{
}

bool setloghost(const char* const)
{
    return false;
}

void setloguid(const int, const int)
{
}

void send_syslog(const std::string&)
{
}

void closelog()
{
}

void dolog(const log_ss, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    std::fputs("[kek] ", stderr);
    std::vfprintf(stderr, fmt, ap);
    std::fputc('\n', stderr);
    va_end(ap);
}

void set_terminal(console* const)
{
}

bool is_terminal_set()
{
    return false;
}

bool toggle_ss_log(const bool, const std::string&)
{
    return false;
}

void set_ss_log(const bool, const log_ss)
{
}

std::string get_ss_mask(const bool)
{
    return {};
}

void disable_all_log_ss(const bool)
{
}

log_ss_type get_log_ss_masks(const bool)
{
    return 0;
}

std::string get_all_available_log_ss_masks()
{
    return {};
}
