# Installing and updating

rtl-swos is experimental software provided without warranty; installing it
can brick the switch, and the authors accept no responsibility for that or
any other consequence (see the [README](../README.md)).

> [!CAUTION]
> Before the first install, read the original firmware out of the flash chip
> with a SOIC-8 clip and a programmer (CH341A or similar) and keep it. That
> needs no soldering, and it is the way back from anything that goes wrong.
> Check that the image was built for your board.

## Flashing the chip

This is the first install on unmanaged switches, and the recovery path for
every switch.

- Disconnect power and open the switch.
- Attach the clip to the flash chip, red wire to pin 1 (the dot). The
  programmer powers the board; if the power LED does not light, check the
  clip.
- Detect the chip with IMSProg, flashrom or similar.
- Read the chip and keep the dump.
- Erase, write `output/rtl-swos.bin`, verify, remove the clip.

## From the original managed firmware

Managed switches can install rtl-swos through the OEM web interface with a
wrapped image:

```
make -C installer
```

Upload `installer/output/rtl-swos_oem_upgrade.bin` on the OEM firmware update
page. The installer copies the whole image, configuration included, so the
switch comes up with the image's startup configuration: by default DHCP on
VLAN 1, 192.168.2.2 until a lease arrives, and telnet with password `1234`.
This is needed once; later updates go in-band.

## In-band updates

A running switch downloads a new image from a TFTP server:

```
switch# copy tftp flash 192.168.10.2 rtl-swos.bin
```

The image is staged at 0x80000, checked against its CRC, and copied into
place by the boot loader when the switch restarts, which it does as soon as
the transfer completes. `show tftp` reports progress and the result. The
startup configuration is kept. Staging needs a flash chip of at least 1 MB.

The startup configuration moves the same way:

```
switch# copy startup-config tftp 192.168.10.2 switch.cfg
switch# copy tftp startup-config 192.168.10.2 switch.cfg
switch# reload
```

## First login

On the serial console (115200 8N1), or over telnet to the management address
with `feature telnet`:

```
rtl-swos-94830a> enable
rtl-swos-94830a# show ip interface brief
```

Change the telnet password under `line vty` and save with `write memory`.

## Factory reset

Holding the reset button for more than 10 s (or for 10–30 s while the switch
boots) copies the image's factory default, the `config.txt` it was built
with, over the startup configuration and restarts.
