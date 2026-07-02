#include "console.h"

#include <cstdio>
#include <tuple>
#include <vector>

console::console(kek_event_t* const stop_event, const int t_width, const int t_height)
    : stop_event(stop_event), t_width(t_width), t_height(t_height)
{
}

console::~console() = default;

void console::begin()
{
}

void console::start_thread()
{
}

void console::stop_thread()
{
}

bool console::poll_char()
{
    return false;
}

int console::get_char()
{
    return -1;
}

std::optional<int> console::wait_char(const int)
{
    return {};
}

std::string console::read_line(const std::string& prompt, const explode_func_t&)
{
    put_string(prompt);
    return {};
}

void console::flush_input()
{
}

void console::emit_backspace()
{
}

void console::put_char(const char c)
{
    std::putchar(c);
}

void console::put_string(const std::string& what)
{
    std::fwrite(what.data(), 1, what.size(), stdout);
}

void console::operator()()
{
}

void console::generate_panel_colors(std::vector<std::tuple<uint8_t, uint8_t, uint8_t>>& to,
                                    const size_t n_leds,
                                    bus* const,
                                    cpu* const,
                                    const uint8_t)
{
    to.assign(n_leds, std::make_tuple(uint8_t(0), uint8_t(0), uint8_t(0)));
}

void console::set_panel_mode(const panel_mode_t pm)
{
    panel_mode = pm;
}

void console::set_panel_brightness(const uint8_t b)
{
    brightness = b;
}

void console::set_blinkenlights_panel(blinkenlights* const p_blinkenlights)
{
    this->p_blinkenlights = p_blinkenlights;
}

void console::set_ddp_panel(ddp* const p_ddp)
{
    this->p_ddp = p_ddp;
}

void console::set_LED_state(const bool)
{
}

void console::pulse_LED()
{
}
