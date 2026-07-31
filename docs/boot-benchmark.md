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

| Profile | Pre-cache | Cache run 1 | Cache run 2 | Cache run 3 | Cache median | Median change |
|---|---:|---:|---:|---:|---:|---:|
| `rsx11mp46` | 41.449 | 41.132 | 41.075 | 41.029 | 41.075 | -0.9% |
| `rt11v5` | 29.769 | 28.440 | 29.190 | 30.685 | 29.190 | -1.9% |
| `unix6` | 5.708 | 4.837 | 4.830 | 16.576 | 4.837 | -15.3% |
| `11mark` | 18.388 | 16.863 | 16.855 | 26.969 | 16.863 | -8.3% |
| `rstsv4` | 2.964 | 2.119 | 2.275 | 12.466 | 2.275 | -23.2% |
| `rsx11m` | 6.100 | 5.682 | 5.744 | 13.034 | 5.744 | -5.8% |
| `rsx11mp46-pidp` | 47.365 | 46.135 | 45.330 | 53.737 | 46.135 | -2.6% |
| `xxdp25` | 4.399 | 4.233 | 4.328 | 12.100 | 4.328 | -1.6% |

The pre-cache suite total was 156.142 seconds. Cache runs 1 and 2 totaled
149.441 and 149.627 seconds. Run 3 totaled 206.596 seconds and developed a
systematic host-side timing slowdown from Unix V6 onward, while guest output
remained correct and the continuous serial log contained no disk, cache,
odd-PC, or emulator-stop errors. Per-profile medians reduce the influence of
that outlier and total 150.447 seconds, a 3.6% improvement over the comparison
baseline.

The three cache suites completed 24 of 24 boots. This is useful stability
evidence, but it does not prove that the earlier intermittent, sticky PiDP
odd-PC restart fault is fixed; retain continuous serial capture during future
suites.
