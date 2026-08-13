// Arduino compiles sketch-root .cpp only. Pull host_lib implementation TUs
// that are not already included by a dedicated sketch-root shim.
#include "host_lib/sd/storage_guard.cpp"
#include "host_lib/log/host_log.cpp"
#include "host_lib/shell/shell_core.cpp"
#include "host_lib/shell/shell_settings.cpp"
#include "host_lib/shell/shell_guest.cpp"
#include "host_lib/shell/shell_media.cpp"
#include "host_lib/telnet/telnet_pipe.cpp"
#include "host_lib/net/wifi_sta.cpp"
#include "host_lib/net/net_task.cpp"
#include "host_lib/net/net_ini.cpp"
#include "host_lib/console/term_adm3a.cpp"
