#pragma once

#include <stddef.h>
#include <stdint.h>

using ShellHandler = void (*)(int argc, char** argv);

void shell_set_out(void (*text)(const char* s));
void shell_out_text(const char* s);
void shell_out_printf(const char* fmt, ...);

// name: primary verb. aliases: nullptr-terminated list, or nullptr.
// pack: help group header (e.g. "File commands"), may be nullptr.
void shell_register(const char* name, ShellHandler fn,
                    const char* help_line,
                    const char* const* aliases = nullptr,
                    const char* pack = nullptr);

void shell_clear_commands();
bool shell_dispatch(int argc, char** argv);  // false = unknown command
void shell_print_help();
