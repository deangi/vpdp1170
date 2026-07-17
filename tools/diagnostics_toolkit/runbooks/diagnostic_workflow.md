# Diagnostic Workflow

This is the repeatable workflow used for the RH70/RP04-RP06 diagnostic pass.

## Board Reset

The tested reset sequence uses the USB serial control lines on `COM18`:

1. Open `COM18` at `115200`.
2. Set `DTR=False`.
3. Set `RTS=True`.
4. Wait `0.5` seconds.
5. Set `DTR=False`.
6. Set `RTS=False`.
7. Wait for XXDP25 to boot. In practice the XXDP prompt appears roughly 25
   seconds after reset release, but the tooling allows a wider timeout.

The serial service implements this as:

```powershell
python tools\diagnostics_toolkit\dtkctl.py serial reboot-board
```

The direct one-shot runner performs the same reset automatically.

## Run A Diagnostic

For service-managed runs:

```powershell
python tools\diagnostics_toolkit\dtk.py run-diagnostic "R ZRJAD0" --config C:\Users\deang\OneDrive\Arduino\ESP32\Freenove_ESP32_S3_Disp\vpdp1170\tools\diagnostics_toolkit\config.example.json --json
```

For direct serial runs with prompt automation:

```powershell
python tools\diagnostics_toolkit\run_xxdp_diagnostic.py "R ZRJDE0" --reply "ENTER DATE:=17-JUL-76\r" --reply "ENTER OPERATOR I.D.:=CODEX\r" --reply "ENTER PARAMETERS:=\r" --output build\captures\zrjde0.txt
```

Common stop markers include `DR>`, `HIT STOP ON DRIVE`,
`PROGRAM WILL HANG`, `HALT instruction`, `HALT regs`, `?MON-F-`,
`END PASS`, `PROGRAM MODE`, and `PROGRAM INITIALIZATION COMPLETE`.

## Telnet Shell Communication

The telnet service connects to `192.168.7.144:23` and exposes local JSON-lines
control on port `11702`.

PDP console to telnet shell:

```powershell
python tools\diagnostics_toolkit\dtkctl.py telnet enter-shell
```

Shell to monitor:

```powershell
python tools\diagnostics_toolkit\dtkctl.py telnet enter-monitor
```

Monitor to shell:

```powershell
python tools\diagnostics_toolkit\dtkctl.py telnet exit-monitor
```

Shell to PDP console:

```powershell
python tools\diagnostics_toolkit\dtkctl.py telnet exit-shell
```

RP operator controls from the telnet shell:

```text
rp stop
rp start
rp status
rp regs
```

If an RP diagnostic asks the operator to press STOP on the drive, use
`rp stop`; if it asks for the drive online again, use `rp start`.

## Build And Flash

Disconnect any service or terminal that owns `COM18` before upload:

```powershell
python tools\diagnostics_toolkit\dtkctl.py serial disconnect
```

Build:

```powershell
& 'C:\Users\deang\AppData\Local\Programs\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe' compile --fqbn 'esp32:esp32:esp32s3:CDCOnBoot=cdc,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,PSRAM=opi' --libraries 'C:\Users\deang\OneDrive\Arduino\libraries' --build-path "$env:TEMP\vpdp1170-arduino-build" .
```

Flash:

```powershell
& 'C:\Users\deang\AppData\Local\Programs\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe' upload -p COM18 --fqbn 'esp32:esp32:esp32s3:CDCOnBoot=cdc,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,PSRAM=opi' --input-dir "$env:TEMP\vpdp1170-arduino-build" .
```

After flashing, perform a clean serial reset before trusting diagnostic output.

## RP/RH70 Notes From This Pass

- `ZRJBD0` originally looped on unexpected RP attention interrupts.
- Clearing reset-time `RPAS` and clearing stale interrupt-request state when
  the final attention bit is cleared stopped the repeated failure.
- Drive-specific reads for selected units `1-7` now report absent instead of
  mirroring RP0, so diagnostics no longer see phantom RP06 drives.
- `ZRJLB0`, `ZRJMB0`, `ZRJNA0`, and `ZRJOB0` are RP07-only on this XXDP25
  pack.
- `ZRJCB0` requires physical head-alignment hardware.
- `ZRJED0` and `ZRJFA0` are dual-controller tests and are not applicable to the
  current single-controller RP0 setup.
