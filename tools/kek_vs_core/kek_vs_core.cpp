// Minimal Visual Studio harness for the kek PDP-11/70 core.
//
// This intentionally starts a little wider than "CPU only" because kek's bus
// owns PDP-visible device wiring. Once it builds and runs, we can trim this
// down to the exact subset needed by vpdp1170.

#include <atomic>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <exception>

#include "bus.h"
#include "cpu.h"
#include "memory.h"
#include "mmu.h"

int main()
{
    try {
    std::atomic_uint32_t event { EVENT_NONE };

    bus b;
    b.set_memory_size(DEFAULT_N_PAGES);

    cpu c(&b, &event);
    b.add_cpu(&c);

    memory* ram = b.getRAM();
    mmu* mmu_ = b.getMMU();

    if (!ram || !mmu_) {
        std::fprintf(stderr, "kek_vs_core: bus did not create RAM/MMU\n");
        return 2;
    }

    c.reset();
    c.setPC(01000);

    // Tiny PDP-11 loop:
    //   001000: MOV #5,R0
    //   001004: MOV #7,R1
    //   001010: ADD R0,R1
    //   001012: BR .
    b.write_physical(01000, 0012700);
    b.write_physical(01002, 0000005);
    b.write_physical(01004, 0012701);
    b.write_physical(01006, 0000007);
    b.write_physical(01010, 0060001);
    b.write_physical(01012, 0000777);

    for (int i = 0; i < 8; ++i) {
        const bool ok = c.step();
        std::printf("step=%d ok=%d PC=%06o PSW=%06o R0=%06o R1=%06o inst=%llu\n",
                    i + 1,
                    ok ? 1 : 0,
                    c.getPC(),
                    c.getPSW(),
                    c.get_register(0),
                    c.get_register(1),
                    static_cast<unsigned long long>(c.get_instructions_executed_count()));
        if (!ok)
               break;
       }

       std::fflush(stdout);
       std::_Exit(0);
    }
    catch (const std::exception& e) {
        std::fprintf(stderr, "kek_vs_core exception: %s\n", e.what());
        return 1;
    }
    catch (...) {
        std::fprintf(stderr, "kek_vs_core unknown exception\n");
        return 1;
    }
}
