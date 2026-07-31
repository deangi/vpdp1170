# Operating-system boot benchmark

`tools/benchmark_boot_times.py` measures guest boot time by installing a named
`pdpconfig-*.ini` through the Telnet management shell, issuing an emulator
reset, and waiting for an operating-system-specific completion prompt. It can
hold the USB serial port for the entire suite so emulator diagnostics are not
lost if a guest fails before Telnet produces useful output.

## Standard Freenove invocation

```powershell
python tools\benchmark_boot_times.py `
  --host 192.168.7.144 `
  --os all --runs 1 --show-output --continue-on-error `
  --timeout 120 `
  --serial-port COM18 --serial-baud 115200 --serial-settle 25 `
  --output boot-benchmark-results
```

Opening COM18 reboots the Freenove board. The script opens it once, waits for
`--serial-settle`, and holds it continuously until the suite completes. Each
output directory contains `boot-times.csv`, one Telnet transcript per profile,
and a timestamped continuous serial log. `pyserial` is required when
`--serial-port` is used.

For each profile the script enters the Telnet management shell, removes
`/pdpconfig.ini`, copies the selected variant to `/pdpconfig.ini`, sends
`reset` followed by `exit`, and starts the timer. Profiles run in this order:

1. `rsx11mp46`
2. `rt11v5`
3. `unix6`
4. `11mark`
5. `rstsv4`
6. `rsx11m`
7. `rsx11mp46-pidp`
8. `xxdp25`

## Completion rules

| Profile | Completion condition |
|---|---|
| `rt11v5`, `xxdp25` | `.` prompt followed by one quiet second |
| `unix6` | `login: ` |
| `rstsv4` | Second `Ready` prompt, followed by one quiet second |
| `rsx11m` | `PLEASE ENTER TIME AND DATE (HR:MN DD-MMM-YY) [S]:` |
| `11mark` | `Please enter time and date (HH:MM DD-MMM-YY) [S]:` |
| `rsx11mp46` | `>@DL:[1,2]STARTUP`, followed by one quiet second |
| `rsx11mp46-pidp` | `Please enter time and date (HH:MM DD-MMM-YYYY) [S T:1M]:` |

For quiet-prompt profiles, the recorded boot time excludes the confirmation
second. RSTS supplies its startup answers automatically. The Unix V6 variant
is `/pdpconfig-unixv6.ini`.

## Shared multi-block cache measurements

The immediately preceding all-pass pre-cache suite is the comparison baseline.
The cache update replaced the per-controller 32 KB read buffers with eight
shared, aligned 8 KB LRU blocks (64 KB total in PSRAM). Blocks are invalidated
on reset, mount, dismount, reopen, and intersecting writes.

All times below are seconds to the configured completion condition.

| Profile | Pre-cache | Cache run 1 | Cache run 2 | Cache run 3 | Cache run 4 | Initial median | Median change |
|---|---:|---:|---:|---:|---:|---:|---:|
| `rsx11mp46` | 41.449 | 41.132 | 41.075 | 41.029 | 48.590 | 41.075 | -0.9% |
| `rt11v5` | 29.769 | 28.440 | 29.190 | 30.685 | 39.168 | 29.190 | -1.9% |
| `unix6` | 5.708 | 4.837 | 4.830 | 16.576 | 13.468 | 4.837 | -15.3% |
| `11mark` | 18.388 | 16.863 | 16.855 | 26.969 | 24.215 | 16.863 | -8.3% |
| `rstsv4` | 2.964 | 2.119 | 2.275 | 12.466 | 11.571 | 2.275 | -23.2% |
| `rsx11m` | 6.100 | 5.682 | 5.744 | 13.034 | 17.844 | 5.744 | -5.8% |
| `rsx11mp46-pidp` | 47.365 | 46.135 | 45.330 | 53.737 | 52.572 | 46.135 | -2.6% |
| `xxdp25` | 4.399 | 4.233 | 4.328 | 12.100 | 11.369 | 4.328 | -1.6% |

The pre-cache suite total was 156.142 seconds. Cache runs 1 and 2 totaled
149.441 and 149.627 seconds. Run 3 totaled 206.596 seconds and developed a
systematic timing slowdown from Unix V6 onward. Run 4 totaled
218.797 seconds and was in the slow timing regime from its first profile.
Guest output remained correct and all continuous serial logs contained no
disk, cache, odd-PC, or emulator-stop errors. A long-running host Arduino CLI
process was present during the first slow measurements, but later testing
showed that host contention was not sufficient to explain the behavior.

The `Initial median` column is the three-run median calculated before run 4;
it reduces the influence of run 3 and totals 150.447 seconds, a 3.6%
improvement over the comparison baseline. Runs 3 and 4 demonstrate that the
system has two distinct timing regimes, so the apparent cache speedup should
be treated as directional.

The four cache suites completed 32 of 32 boots. This is useful stability
evidence, but it does not prove that the earlier intermittent, sticky PiDP
odd-PC restart fault is fixed; retain continuous serial capture during future
suites.

## Persistent fast and slow board states

A full Freenove board reset restored the fast regime. The first complete suite
after that reset (cache run 5) passed all eight profiles in 150.339 seconds,
consistent with cache runs 1 and 2. After further operation, front-panel menu
operations involving the SD card became noticeably sluggish. A Telnet-only
suite was then run without opening COM18, because opening COM18 itself reboots
the board and would have destroyed the state under investigation. This run
(cache run 6) passed all profiles but took 220.754 seconds:

| Profile | Run 5 after board reset | Run 6 in observed slow state | Change |
|---|---:|---:|---:|
| `rsx11mp46` | 41.126 | 53.253 | +29.5% |
| `rt11v5` | 29.777 | 38.462 | +29.2% |
| `unix6` | 5.107 | 14.093 | +176.0% |
| `11mark` | 16.933 | 24.126 | +42.5% |
| `rstsv4` | 2.151 | 11.828 | +449.9% |
| `rsx11m` | 5.753 | 15.789 | +174.5% |
| `rsx11mp46-pidp` | 45.095 | 51.409 | +14.0% |
| `xxdp25` | 4.397 | 11.794 | +168.2% |
| **Suite total** | **150.339** | **220.754** | **+46.8%** |

The fast runs cluster near 150 seconds, while the slow runs total 206.596,
218.797, and 220.754 seconds. The state typically appears after roughly one
to three complete benchmark suites and is cleared by a full board reset.
Because the front-panel SD operations slow down concurrently, and run 6
preserved the board state by avoiding COM18, this is an on-board accumulated
condition rather than merely a host timer, Telnet, or USB serial artifact.

The cause remains unknown. Heap or PSRAM fragmentation, a memory/resource
leak, accumulated FreeRTOS work or queue state, lock contention, or degraded
SD/SPI state are plausible. Heap fragmentation is not yet established, and
the successful mounts and boots plus earlier file-handle checks argue against
simple file-handle exhaustion. The larger proportional penalty on short,
disk-heavy boots and the simultaneous SD-menu sluggishness point more strongly
to the storage path or a shared resource used by it than to a uniform loss of
PDP-11 CPU emulation speed.

If investigation resumes, record these values at boot, between every profile,
and when the slow state first appears:

- Internal RAM and PSRAM free bytes, minimum-ever free bytes, and largest free
  block (the largest-block/free-space ratio will help identify fragmentation).
- SD read latency and throughput for fixed-size uncached reads, including
  maximum latency and error/retry counts.
- Disk-cache hit, miss, refill, and invalidation counts and refill latency.
- FreeRTOS task stack high-water marks, queue depths, and time spent waiting
  for the SD/storage lock.

Run 5 results are in `boot-benchmark-results-multicache-run5`;
the preserved slow-state results are in
`boot-benchmark-results-multicache-run6-current-state`.

## Guest RAM size on emulator reset

Two related host-memory issues inflated boot times when switching
`pdpconfig-*.ini` profiles:

1. `apply_runtime_pdp_config()` did not call `pdp_core::set_target_memory_kw()`,
   so the reset banner printed the new INI's `mem_size_kw` while guest RAM kept
   the previous profile's size.
2. Each size change freed and `ps_malloc`'d PSRAM, and every cold boot
   `memset` the entire allocated buffer (up to ~4 MB).

The fix allocates the full 2 MW slab (512 × 8 KB) once at kek init, reuses that
capacity for every profile, applies only a logical size change on reset, and
clears only the configured size. After flashing that change, a Telnet-only
suite totaled **127.553 seconds** with `rt11v5` at **7.805 seconds** (back in
the expected 8–9 s range):

| Profile | Telnet before mem fix | After mem_size apply | After 2MW reuse |
|---|---:|---:|---:|
| `rsx11mp46` | 41.206 | 51.492 | 40.627 |
| `rt11v5` | 30.158 | 18.344 | 7.805 |
| `unix6` | 4.838 | 15.304 | 4.802 |
| `11mark` | 16.820 | 24.112 | 17.651 |
| `rstsv4` | 2.291 | 13.683 | 1.979 |
| `rsx11m` | 5.769 | 13.592 | 5.648 |
| `rsx11mp46-pidp` | 45.384 | 53.628 | 44.592 |
| `xxdp25` | 4.401 | 16.627 | 4.449 |
| **Suite total** | **150.867** | **206.782** | **127.553** |

The middle column landed in the slow board-state regime; the post-reuse suite
is faster than the earlier ~150 s “fast” cluster because smaller guests no
longer pay a full multi-megabyte clear (or a free/realloc) on every reset.

Results directories:

- `boot-benchmark-results-telnet-only` — pre-fix baseline
- `boot-benchmark-results-telnet-only-after-memfix` — after `mem_size_kw` apply only
- `boot-benchmark-results-telnet-only-after-memreuse` — after 2MW allocate-once reuse
