# rtl-swos

Command-line switch firmware for RTL8372/RTL8373 based 2.5 GBit switches.

rtl-swos is a hard fork of RTLPlayground. It drops the web interface and
manages the switch through an industry-standard modal CLI on the serial
console and over telnet, with a block-structured running configuration
that is saved to flash and replayed at boot:

```
hostname lab-sw1
!
vlan 10
 name home
vlan 20
 name work
!
interface ethernet 1/1
 description uplink
 switchport mode trunk
 switchport trunk allowed vlan 10,20
!
interface ethernet 1/2
 switchport access vlan 20
!
interface vlan 10
 ip address 192.168.0.25 255.255.254.0
!
ip default-gateway 192.168.0.1
feature telnet
!
end
```

Features, as far as the hardware supports them:
- VLANs: access and trunk ports, native VLAN, allowed lists, protected ports
- Per-port speed, duplex, MTU (jumbo frames), Energy Efficient Ethernet and
  shutdown, with link-partner information
- Link aggregation (port-channels), static or LACP, with a configurable hash
- Port mirroring (one session, several sources)
- Ingress and egress rate limiting
- Spanning tree (RSTP/STP, simplified; read [doc/stp.md](doc/stp.md) first)
- IGMP snooping
- SFP+ module information and diagnostics
- Management interface on any VLAN, static or DHCP; remote syslog
- In-band firmware and configuration transfer over TFTP
- Show commands for interfaces, counters, VLANs, MAC table, port-channels,
  spanning tree and transceivers

The [command line reference](doc/cli.md) lists every command.

The firmware supports devices with
- 4 2.5GBit ports + 2 SFP+ ports
- 5 2.5GBit ports + 1 SFP+ port
- 8 2.5GBit ports + 1 SFP+ port

Devices sold usually share a common design, but LED wiring and colours
differ. The tested boards are listed in
[Supported devices](doc/supported_devices.md).

> [!CAUTION]
> The firmware lacks the proprietary loop prevention of the original
> managed firmware. Install it only if you can back up and restore the
> flash chip with a SOIC-8 clip and a programmer such as a CH341A. No
> soldering is needed for that.

## Building

Build requirements (Debian 12/13; sdcc 4.5 or newer is needed, Ubuntu
24.04 ships an older one):
```
sudo apt install make gcc sdcc python3 zlib1g-dev
```

<details>
<summary>Building with Docker</summary>

```
docker build -t rtl-swos-dev .
docker run --rm -v $(pwd):/workspace rtl-swos-dev make MACHINE=DEFAULT_8C_1SFP
```

The image appears in `output/` on the host. The source directory is
mounted, so edits to `machine.h` or `config.txt` take effect on the next
`make`.
</details>

Select the board in `machine.h`, or pass it on the command line, and
build:
```
make MACHINE=SWTGW218AS
```

The image is written to `output/<MACHINE>/rtl-swos-<version>-<MACHINE>.bin`,
with a link at `output/rtl-swos.bin`. It is a complete 512 KB flash image:
it can be written to the flash chip directly or loaded in-band.

The image carries an initial startup configuration, `config.txt` unless
`CONFIG=` names another file. It is also the configuration a factory reset
(reset button held for more than 10 s) restores. The committed `config.txt`
is a generic default:

- an unmanaged-style switch: all ports access ports in VLAN 1, STP off;
- the management interface on VLAN 1 as a DHCP client. Until a lease
  arrives the switch answers on the built-in 192.168.2.2/24 (gateway
  192.168.2.22), and `show ip interface brief` says `dhcp, no lease yet`;
- telnet on, with the default password `1234`.

> [!WARNING]
> With the default configuration anyone on the LAN can log in with `1234`.
> Set a password under `line vty` and `write memory`, or build with your
> own configuration.

For a site-specific image, keep your configuration outside the tree and
point the build at it:
```
make MACHINE=SWTGW218AS CONFIG=../site.cfg
```

Host-side tests of the CLI, configuration model and serializer run without
hardware:
```
make -C test
```

## Installing

### Flashing the chip (first install, and recovery)

This is the only way to install on unmanaged switches, and the way to
recover from a bad image.

- Disconnect power and open the switch. The warranty is gone.
- Attach the SOIC-8 clip to the flash chip (red wire to pin 1, marked by a
  dot). The switch's power LED lights up from the programmer.
- Detect the chip with IMSProg, flashrom or similar.
- **Read and keep a backup of the original firmware.**
- Erase the chip, write `output/rtl-swos.bin`, verify, remove the clip.

### From the original managed firmware

Managed switches can be upgraded from the OEM web interface with a special
image. Build the firmware first, then:
```
make -C installer
```
and upload `installer/output/rtl-swos_oem_upgrade.bin` through the OEM
firmware update page. This is needed once; later updates go in-band. The
installer copies the whole image, configuration included, so the switch
comes up with the image's startup configuration (by default: DHCP on VLAN 1,
telnet on, 192.168.2.2 until a lease arrives).

> [!CAUTION]
> Check that the machine type matches the device before flashing.

### In-band updates

On a running rtl-swos switch, serve the image from a TFTP server and run:
```
switch# copy tftp flash 192.168.10.2 rtl-swos.bin
```
The image is staged in flash, checked, and copied into place by the boot
loader on the next start; the switch reboots when the transfer completes.
`show tftp` reports progress and the result. The startup configuration is
not touched by a firmware update.

The configuration can be moved the same way:
`copy tftp startup-config SERVER FILE` and
`copy startup-config tftp SERVER FILE`.

## Using the switch

Connect a serial adapter to the UART header (115200 8N1), or enable
`feature telnet` and connect to the management address. The telnet
password is `1234` until changed under `line vty`.

```
rtl-swos-94830a> enable
rtl-swos-94830a# show interfaces status
Port      Name              Status      Vlan     Speed  Type
--------  ----------------  ----------  -------  -----  ------
Eth1/1                      notconnect  1        auto   copper
Eth1/2                      connected   1        1000   copper
...
rtl-swos-94830a# configure terminal
rtl-swos-94830a(config)# interface ethernet 1/3-5
rtl-swos-94830a(config-if-range)# switchport access vlan 20
% VLAN 20 did not exist, created it
rtl-swos-94830a(config-if-range)# end
rtl-swos-94830a# write memory
```

`?` shows what may follow at any point, Tab completes, and keywords can be
abbreviated. Changes apply immediately; `write memory` makes them survive
a reload.

### Configurations from RTLPlayground

The startup configuration of RTLPlayground used a flat command syntax
(`vlan 10 home 2 3 9t`, `pvid 1 20`, ...). Convert it before loading it:
```
tools/convert-legacy-config.py old.cfg > new.cfg
```
The converter marks anything without an exact equivalent with a
`! NOTE:` comment. `make -C test && test/build/test_replay new.cfg` replays
the result through the real CLI on the host and fails on any error.

## Other documents

- [Command line reference](doc/cli.md)
- [RTL8372/3 feature support](doc/hardware.md)
- [CPU port](doc/CpuPort.md)
- [L2 learning](doc/l2.md)
- [VLAN](doc/vlan.md)
- [Link aggregation](doc/link_aggregation.md)
- [Mirroring](doc/mirroring.md)
- [Bandwidth control](doc/bandwidth.md)
- [Spanning tree](doc/stp.md)
- [IGMP (IP multicast)](doc/igmp.md)
- [SFP+ ports](doc/sfp.md)
- [Automation](doc/automation.md)
- [Modifications and flash replacement](doc/mods.md)
- [Ghidra](doc/ghidra.md)
- [XRAM above 0x4000 is not zero-initialized](doc/xram.md) - read before adding `__xdata` state
