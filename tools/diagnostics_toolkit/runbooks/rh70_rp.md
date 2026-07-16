# RH70 / RP04-RP06 Diagnostics

## Current Focus

Diagnostics seen so far:

- `ZRJAD0` - RP04/5/6 mechanical and read-write test
- `ZRJGE0` - RP04/5/6 diskless test, part 1
- `ZRJHE0` - RP04/5/6 diskless test, part 2

## Operator Actions

When the diagnostic reports:

```text
DRIVE IS ON LINE - MOL IS HIGH
HIT STOP ON DRIVE TO GET IT OFF LINE
PROGRAM WILL HANG TESTING MOL TILL MOL IS LOW
```

Enter the telnet management shell and run:

```text
rp stop
```

If the diagnostic later asks for the drive to be online again, run:

```text
rp start
```

Check state with:

```text
rp status
```

## Failure Classes

`NO DRIVES PRESENT, RHAS=0`

Likely RHAS/attention-status behavior. The diagnostic writes error registers
for each unit and expects the corresponding RHAS bit to appear.

`WRONG DATA IN READING OR WRITING HARDWARE REGISTER`

Likely register read/write semantics. In diskless tests, the drive may be
offline while diagnostics write patterns into RH/RP registers and expect them
to read back. Pay close attention to byte writes and live status registers such
as `DS` and `LA`.

`UNEXPECTED RP INTERRUPT`

Likely interrupt enable, attention, command completion, or stale IRQ clearing.
Capture the preceding register failures before changing IRQ behavior.

## Useful Registers

- `176700` - CS1
- `176702` - WC
- `176704` - BA
- `176706` - DA
- `176710` - CS2
- `176712` - DS
- `176714` - ER1
- `176716` - AS
- `176720` - LA
- `176722` - DB
- `176724` - MR
- `176726` - DT
- `176730` - SN
- `176732` - OFR
- `176734` - DC
- `176736` - CC
- `176750` - BAE
