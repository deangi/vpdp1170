#pragma once
#ifndef HOST_LIB_TERM_PERSONALITY_H
#define HOST_LIB_TERM_PERSONALITY_H

// Pluggable console parser. vpdp1170 / v8088 use VT100.
// vZ80 must use ADM-3A only — never enable the VT100/CSI personality there.

enum HostTermPersonality {
  HOST_TERM_VT100 = 0,
  HOST_TERM_ADM3A = 1,
};

#endif  // HOST_LIB_TERM_PERSONALITY_H
