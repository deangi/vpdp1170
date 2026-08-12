// Minimal kek `comm` base methods for vpdp1170 (avoid linking full
// _upstream_kek/comm.cpp and its optional serial/TCP backends).

#include "config.h"
#include <cstring>
#include <string>

#include "_upstream_kek/comm.h"

#if defined(ESP32)
SC16IS752 *comm::ser2_inst_1 { nullptr };
SC16IS752 *comm::ser2_inst_2 { nullptr };

void comm::set_comm(SC16IS752 *const a, SC16IS752 *const b)
{
	ser2_inst_1 = a;
	ser2_inst_2 = b;
}
#endif

comm::comm() {}
comm::~comm() {}

void comm::println(const char *const s)
{
	send_data(reinterpret_cast<const uint8_t *>(s), strlen(s));
	send_data(reinterpret_cast<const uint8_t *>("\r\n"), 2);
}

void comm::println(const std::string & in)
{
	send_data(reinterpret_cast<const uint8_t *>(in.c_str()), in.size());
	send_data(reinterpret_cast<const uint8_t *>("\r\n"), 2);
}
