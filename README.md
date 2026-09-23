# rtl-swos

Command-line firmware for RTL8372/RTL8373 based 2.5 GBit switches.

rtl-swos manages the switch the way enterprise switches are managed: a modal
CLI on the serial console and over telnet, with a block-structured running
configuration that is saved to flash and replayed at boot.

```
rtl-swos-94830a# configure terminal
rtl-swos-94830a(config)# interface ethernet 1/1-2
rtl-swos-94830a(config-if-range)# channel-group 1 mode active
rtl-swos-94830a(config-if-range)# switchport mode trunk
rtl-swos-94830a(config-if-range)# switchport trunk allowed vlan 10,20
rtl-swos-94830a(config-if-range)# end
rtl-swos-94830a# write memory
```

> [!WARNING]
> **rtl-swos is highly experimental.** It is developed and tested on very few
> devices, may misbehave in ways that disrupt your network, and replacing a
> switch's firmware can leave it unbootable ("bricked"). Recovering may
> require opening the device and reflashing the chip by hand. The software
> is provided as is, without warranty of any kind; the authors and
> contributors accept no responsibility or liability for any damage, data
> loss, network outage, bricked hardware or other consequence of using it,
> for any purpose and however it is used. Use it entirely at your own risk.

## Based on RTLPlayground

rtl-swos is a hard fork of [RTLPlayground](https://github.com/logicog/RTLPlayground)
by logicog and its contributors. Nearly everything below the command line
comes from that project: the reverse engineering of the RTL837x, the drivers
for ports, PHYs, SerDes, VLANs, the L2 table, link aggregation, mirroring,
bandwidth control, IGMP and SFP modules, the spanning tree implementation,
the uIP network stack integration, DHCP, the firmware update path, support
for dozens of boards and the documentation of all of it. rtl-swos replaces
the web interface and the console command language with a new CLI and
configuration model, and adds LACP and TFTP transfers. Many thanks to
everyone who built RTLPlayground; their work is in this repository's history.

## Features

- Modal CLI in the familiar style: `enable`, `configure terminal`, submodes,
  abbreviations, `?` help, Tab completion, `no` forms, `^` error markers,
  interface ranges, EXEC commands in configuration modes without `do`
- VLANs: access and trunk ports, native VLAN, allowed lists, protected ports
- Per-port speed, duplex, MTU, Energy Efficient Ethernet, rate limiting
- Link aggregation, static or LACP, with a configurable hash
- Spanning tree (RSTP/STP), port mirroring, IGMP snooping
- SFP+ module information and diagnostics
- Management on any VLAN, static or DHCP; telnet with an optional TOTP
  second factor; remote syslog; DNS resolver; NTP client
- In-band firmware and configuration transfer over TFTP
- `show` commands for interfaces, counters, VLANs, MAC table, port-channels,
  LACP, spanning tree and transceivers

There is no web interface and no SSH.

## Hardware

RTLPlayground supports switches with 4 + 2, 5 + 1 and 8 + 1 ports; see
[Supported devices](doc/supported_devices.md). rtl-swos keeps that board
support but has so far been tested on a SWTGW218AS (8 + 1 ports, 2 MB flash).
In-band updates need at least 1 MB of flash.

> [!CAUTION]
> Install only if you can back up and restore the flash chip with a SOIC-8
> clip and a programmer, and only on hardware you can afford to lose. See
> [Installing](doc/installing.md).

## Quick start

```
sudo apt install make gcc sdcc python3   # sdcc >= 4.5
make MACHINE=SWTGW218AS                              # output/rtl-swos.bin
make -C test                                         # host tests
```

Flash `output/rtl-swos.bin` to the chip, or install it from the OEM web
interface with `make -C installer`. The default configuration makes the
switch a DHCP client on VLAN 1 (192.168.2.2 until a lease arrives) with
telnet on and the password `1234`; change the password under `line vty`.
Build with `make CONFIG=your.cfg` to start from your own configuration.

## Documentation

Using rtl-swos:
- [Building](doc/building.md): toolchain, board selection, the baked-in
  configuration, host tests
- [Installing and updating](doc/installing.md): flashing, OEM install,
  in-band updates, factory reset
- [Command line reference](doc/cli.md)
- [Migrating from RTLPlayground](doc/migrating.md)
- [Time, DNS and TOTP](doc/time.md)
- [Automation](doc/automation.md): scripting over telnet and TFTP

Features, and how the hardware does them:
- [VLAN](doc/vlan.md), [L2 learning](doc/l2.md), [Link aggregation and LACP](doc/link_aggregation.md)
- [Spanning tree](doc/stp.md), [Mirroring](doc/mirroring.md), [Bandwidth control](doc/bandwidth.md)
- [IGMP](doc/igmp.md), [SFP+ ports](doc/sfp.md)

Hardware and development:
- [RTL8372/3 feature support](doc/hardware.md), [CPU port](doc/CpuPort.md), [GPIO](doc/gpio.md)
- [Supported devices](doc/supported_devices.md), [Modifications and flash replacement](doc/mods.md)
- [XRAM above 0x4000 is not zero-initialized](doc/xram.md), [Ghidra](doc/ghidra.md)

## License

MIT, like RTLPlayground; see [LICENSE](LICENSE). The license's warranty
disclaimer and limitation of liability apply in full; see the warning at
the top of this file.
