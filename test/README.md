# Host unit-test harness

Compile and test individual firmware translation units on the build host with
**gcc + AddressSanitizer + UBSan**: no SDCC, no flashing, no hardware. It is
the fast inner loop for the CLI, the configuration model and the drivers
behind them: edit, `make`, see red/green in seconds instead of a
flash-and-reboot cycle.

## Run
```
cd test
make            # build + run all tests (ASan/UBSan on)
make clean
build/test_replay file.cfg [expected...]   # replay a config through the CLI
```
Exit status is non-zero if any check fails, a sanitizer trips, or the watchdog
fires — so it drops straight into CI.

## How it works
- **`sdcc_shim.h`** (force-included) erases SDCC 8051 keywords (`__xdata`,
  `__code`, `__banked`, …) so firmware sources compile under host gcc. It changes
  no logic — the code under test is byte-for-byte the firmware source.
- **`hw_mock.c` / `hw_mock.h`** simulate the ASIC edge: a flat register file
  behind `reg_read`/`reg_write` and the `SFR_DATA_*` registers, with a table
  engine that performs a VLAN or L2 operation the moment `TBL_CTRL` is written,
  fetches a MIB counter on `STAT_GET` and drops entries on `L2_TBL_FLUSH_CTRL`.
  Nothing ever reports busy, so a polling loop runs once. The header states the
  VLAN and L2 entry layouts independently of the firmware, so a test can hold
  what `rtl837x_port.c` writes against what it reads back.
- **`env_tables.c`** and **`env_cli.c`** carry the globals and edge functions
  the firmware modules link against (PHY, DHCP, syslog, telnet, flash, frame
  output), recording calls so tests can assert on them; **`stub/`** stands in
  for `version.h`, which only a firmware build generates.
- **`support.c` / `support.h`** mock the hardware edges: the 16-byte serial ring
  (`sbuf`), the command/history buffers, and the character-output sink
  (`write_char` etc.). Buffers are sized **exactly** as on target, so ASan
  redzones catch the same off-by-one overflows the 8051 hits.
- Builds define **`SWOS_HOST_TEST`**, which hides the firmware's libc-named
  prototypes (`memset`/`strlen`/…) in `rtl837x_common.h` so they don't clash with
  glibc. Argument order matches libc, so on-host callers transparently use the C
  library. This guard is compiled out of normal firmware builds — zero on-target
  effect.
- A **SIGALRM watchdog** (`watchdog_arm`) turns an infinite-loop bug into a fast
  test failure instead of a hung runner.

## Current coverage
Every binary links the same environment (`CORE_ENV` in the Makefile): the
command editor, the CLI engine and actions, the configuration model, the
running-config serializer, the show commands, LACP and the port driver,
compiled unmodified from the firmware tree.

| Test binary | Focus |
|-------------|-------|
| `test_cmd_editor` | line editor: full-line hang, buffer bounds, entry and backspace |
| `test_cli` | modes, abbreviation, help, completion, errors, every configuration command, running config round trip, boot replay, sessions, show output, interface ranges, spanning tree, port-channels, LACP against a simulated partner |
| `test_port_tables` | VLAN entry layout and round trip, PVID register sharing, static multicast and management entries, per-port flush, trunk membership and hash seed |
| `test_replay` | replays a configuration file as the switch does at boot; `make run` uses it on `config.txt` and on the converted example configuration |

## Adding a test
Add a `test_<topic>` function to `test_cli.c`, or a new `test_<topic>.c` with
its own `main()` and a line in `TESTS`; `CORE_ENV` provides the environment.
Add missing edge functions to `env_cli.c` only as the linker asks for them.
