# XRAM above 0x4000 is not zero-initialized

## Symptom

On an SWTGW218AS running a build whose ordinary `__xdata` variables
extended past `0x4000`, `write memory` refused with "TFTP transfer in
progress" on every boot, and `copy tftp flash` refused with "TFTP transfer
already in progress". No transfer was running. The in-band update path had
locked itself out and the switch had to be reflashed over SPI.

## Cause

The TFTP client's state struct had no explicit initializer and relied on
the C guarantee that static storage starts at zero. It sat at `0x4132`.

SDCC's startup code (`__mcs51_genXRAMCLEAR`, run from GSINIT before
`main()`) is linked, runs, and is correct: it zeroes the whole XSEG,
`0x0001` up to the end of the segment. Yet after boot:

| Address | Content after boot |
|---|---|
| `0x0132`, `0x3000` | `00 00 00 ...` (cleared) |
| `0x4010`, `0x4132` | random bytes (not cleared) |
| `0x7000`, `0x8132` | random bytes (outside XSEG, expected) |

`0x4132` is not a mirror of `0x0132`, and writes above `0x4000` do persist
at run time (the region pattern-tests clean, see below). So the part of
XRAM at and above `0x4000` is usable once the firmware runs, but it is not
usable yet when GSINIT clears it. Everything placed there starts with
power-on garbage instead of zero, and initialized variables (XISEG) placed
there would miss their initial values the same way.

The build had only just crossed the line: ordinary xdata ended at `0x396C`
before, and at `0x49C1` after the telnet output buffer grew and the config
buffer moved. The upstream firmware never kept long-lived state above
`0x4000`, only short-lived scratch that is written before it is read, so
the problem stayed invisible there.

What makes the upper region live, and when, is not known.

## Current rule (enforced)

- Every ordinary `__xdata` variable stays below `XRAM_LOW_LIMIT`
  (`0x4000`, `rtl837x_common.h`). The Makefile fails the build when
  `s_XISEG + l_XISEG` exceeds `0x4000`. Do not "fix" such a failure by
  raising the limit.
- Large scratch buffers that never rely on their initial contents are
  pinned above the limit with `__at`, and their `extern` declarations carry
  the same `__at`:
  - `cfg_buf`: `0x4000`, 4 KB (`runcfg.c`)
  - `telnet_outbuf`: `0x5000`, 6 KB (`telnetd.c`)
- Modules still get an explicit `*_init()` for their state, in the upstream
  style, instead of relying on zero-initialization (`tftp_init()` runs as
  the last step of boot).
- As defense in depth, `tftp_busy()` only reports a transfer in progress
  when a live UDP connection is bound to the TFTP client port. Stale state
  is cleared instead of blocking updates.

The cost is a 16 KB budget for ordinary variables; about 8.8 KB of it is
in use. The upper 48 KB stays available for explicitly placed scratch
buffers.

Debug commands:

- `xget <hexaddr>` dumps 16 bytes of XRAM.
- `xtest <hex4 addr> <hex4 len>` runs a destructive pattern test and
  refuses addresses below `0x4000`.

Verified good so far: `0x4000`-`0x4FFF` and `0x6800`-`0x6FFF`, 0 bad bytes.

## Why not simply zero the upper region ourselves during boot?

That becomes the better fix once the root cause is known. Until then it is
weaker than the rule above:

1. The safe moment is unknown. GSINIT's clear runs immediately before
   `main()` and has no effect, so a clear at the top of `main()` may not
   work either. A clear late in boot could wipe state an earlier init
   already wrote there.
2. Zeroing alone is not enough. Initialized variables are copied from code
   space by the same startup code, so the XISEG copy would have to be
   repeated for the upper range too, at the same safe moment.
3. A misplaced clear fails silently and brings back intermittent bugs like
   this one, whereas the rule fails the build.

## Path forward, when more than 16 KB of ordinary variables is needed

Find out when the region becomes live, without a UART:

1. Add a probe that writes a known pattern to `0x4000` and reads it back.
   Log pass/fail into a small array below `0x4000`.
2. Call it at several points of boot: the first line of `main()`, after
   clock setup, after `flash_init()`, after the NIC and switch init, and so
   on.
3. After boot, read the log with `xget`. The first passing probe marks the
   point from which the region is live.

If the region is live at the first line of `main()`, a clear there, plus
the XISEG copy for the upper range, is safe. The limit can then be lifted
and this document updated.
