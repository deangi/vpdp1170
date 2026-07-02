# kek Visual Studio Porting Harness

This folder is the desktop bridge between upstream `kek` and the Arduino
`vpdp1170` sketch.

The goal is not to build the full `kek` application. The goal is to build a
small, Visual Studio-friendly executable that exercises the PDP-11/70 core
pieces we need to port:

- CPU
- MMU
- memory
- bus / I/O page dispatch
- minimal console and interrupt plumbing
- selected devices used by early boot tests

The harness references source files from:

```text
../../_upstream_kek
```

## Why This Step Exists

Porting directly into Arduino would make every issue ambiguous: C++ language
differences, ESP32 memory constraints, host-device callbacks, and CPU/MMU
correctness would all fail together.

This harness lets us first get a normal Windows desktop build working under
Visual Studio 2026, then progressively reduce dependencies until the same
core shape can be embedded in the Arduino sketch.

## Build

From this folder:

```powershell
cmake -S . -B build
cmake --build build --config Debug
.\build\Debug\kek_vs_core.exe
```

On this machine, Visual Studio 2026 includes CMake here:

```powershell
& 'C:\Program Files\Microsoft Visual Studio\18\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' -S . -B "$env:LOCALAPPDATA\Temp\vpdp1170-kek-build-min"
& 'C:\Program Files\Microsoft Visual Studio\18\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' --build "$env:LOCALAPPDATA\Temp\vpdp1170-kek-build-min" --config Debug
& "$env:LOCALAPPDATA\Temp\vpdp1170-kek-build-min\Debug\kek_vs_core.exe"
```

Keeping the generated Visual Studio tree out of OneDrive avoids very slow
configure/build passes while OneDrive scans the generated project files.

If Visual Studio 2026 needs an explicit generator, use:

```powershell
cmake -S . -B build -G "Visual Studio 18 2026"
cmake --build build --config Debug
```

## First Target

The first executable should:

1. Allocate a `kek` bus and memory object.
2. Attach `kek` CPU and MMU.
3. Set PC and PSW.
4. Deposit a tiny PDP-11 program into memory.
5. Execute a small number of instructions.
6. Print registers and instruction count.

That proves the core can be built independently before any ESP32 device
integration begins.

## Device Selection Rule

PDP-visible hardware should come from `kek` where available. ESP32-visible
services stay in `vpdp1170`.

See:

```text
../../docs/device-plan.md
```
