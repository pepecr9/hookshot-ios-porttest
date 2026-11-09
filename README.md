# Hookshot porttest (ARM64 / iOS)

Checks whether Hookshot's AArch64->C lifter (a64-kit `a64emit` + `nx2/rtgen.py` 64-bit window, Hookshot commit
`b2765ba`) produces C that behaves like the CPU when compiled for ARM64 (macOS arm64 and iOS arm64), and probes
whether iOS can reserve Hookshot's fixed guest window and how much memory an app may use.

- `insns.S` / `insns.bin`: 209 hand-written instructions (no game code).
- `gen.py`: lifts them with Hookshot's lifter, writes the lifted C and native reference stubs.
- `porttest.c`: native-vs-lifted comparison, VA probe, memory probe. `ios/`: unsigned test app.

No game files, keys or code derived from a game are used. GPL-3.0-or-later (links GPL code from Hookshot).
