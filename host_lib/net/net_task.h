#pragma once
#include <stdint.h>

using HostNetPollFn = void (*)();

bool host_net_task_add(HostNetPollFn fn);
void host_net_task_set_period_ms(uint32_t ms);
bool host_net_task_start(const char* name = "net", uint32_t stack = 8192,
                         int prio = 2, int core = 0);
void host_net_task_poll_once();
