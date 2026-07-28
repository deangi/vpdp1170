# vpdp1170 kek port staging

This directory holds the Arduino-side PDP-11/70 adapter. The sketch-root
`pdp_core_kek.cpp` includes `pdp_core_kek_impl.cpp` so Arduino (which does
not compile sketch subdirectories) can link the adapter.

Guest execution is always the kek PDP-11/70 path. The inherited
vpdp1140/sam11 scaffold lives under `legacy_sam11/` for reference only.

Upstream sources are pulled in by sketch-root `kek_src_*.cpp` wrappers
(one translation unit per upstream file).

`kek_src_optional_device_stubs.cpp` satisfies linker references from kek
`bus.cpp` for optional devices not yet fully ported (DC11, DZ11, DEQNA,
TM11).
