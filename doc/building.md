# Building

## Requirements

rtl-swos is built with SDCC (4.5 or newer) for the 8051 core of the
RTL8372/RTL8373, plus a host C compiler for the image tools. On Debian 12/13:

```
sudo apt install make gcc sdcc python3
```

Ubuntu 24.04 ships an SDCC that is too old.

A Dockerfile gives a reproducible environment instead:

```
docker build -t rtl-swos-dev .
docker run --rm -v $(pwd):/workspace rtl-swos-dev make MACHINE=DEFAULT_8C_1SFP
```

## Building an image

Select the board, either by uncommenting its `#define MACHINE_...` line in
`machine.h` or on the command line:

```
make MACHINE=SWTGW218AS
```

The result is `output/<MACHINE>/rtl-swos-<version>-<MACHINE>.bin`, linked as
`output/rtl-swos.bin`: a complete 512 KB flash image with a trailing CRC. It
can be written to the flash chip or loaded in-band (see
[Installing](installing.md)). `make machine_check` compiles the machine
tables of every board.

## The configuration baked into the image

The image carries an initial startup configuration in two flash sectors:
0x70000 is the startup configuration the switch boots with, 0x6f000 the
factory default a reset restores. Both come from `config.txt`, or from the
file `CONFIG=` names:

```
make MACHINE=SWTGW218AS CONFIG=../site.cfg
```

The committed `config.txt` is a generic default: all ports in VLAN 1, the
management interface a DHCP client on VLAN 1 (192.168.2.2/24 until a lease
arrives) and telnet on with the password `1234`. Keep site configurations
outside the tree. An in-band firmware update does not touch the startup
configuration of a running switch.

## Host tests

The CLI engine, the configuration model, the serializer, the show commands,
LACP and parts of the port driver compile unmodified for the host against a
register mock:

```
make -C test
```

The run also replays `config.txt` and the converted example configuration
through the real CLI and fails on any error line.

## Memory rules

The 8051 has 256 bytes of internal RAM, and none of it is spare: every
function keeps its locals in `static __xdata` and takes at most one register
parameter, or the link fails with `Could not get N consecutive bytes in
internal RAM for area OSEG`. External RAM above 0x4000 is not cleared at
boot, and the build refuses ordinary `__xdata` above that limit; see
[XRAM](xram.md). Code is banked (HOME plus three 48 KB banks); a call into
another bank must go to a `__banked` function, and a string must be read
from the bank it lives in.
