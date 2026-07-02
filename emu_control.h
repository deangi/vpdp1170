#pragma once

namespace emu_control {

void init();
bool submit(const char* command);
void poll();
bool consume_reboot_request();

}  // namespace emu_control
