# Automation

The switch is automated over its management interface with telnet (for
commands) and TFTP (for firmware and startup configs). The switch is the TFTP
client: it fetches from and uploads to a TFTP server you run, e.g. `tftpd-hpa`
or `dnsmasq --enable-tftp`.

## Telnet

The telnet server is off by default. Enable it once on the serial console:

```
switch# configure terminal
switch(config)# feature telnet
switch(config)# end
switch# write memory
```

There is one session at a time. The login password is `1234` unless changed
under `line vty` with `password <word>`; `exec-timeout <min> [<sec>]` sets the
idle timeout (default 10 minutes, `0 0` disables it). A session starts in
EXEC mode, so send `enable` before privileged commands. The output of one
command is limited to about 6 KB; longer output ends with `[output truncated]`.

No telnet client is needed. The server accepts lines terminated by LF, and
the option negotiation it sends can be ignored, so `nc` is enough:

```sh
#!/bin/sh
SWITCH=192.168.0.25
PASSWORD=1234

{
    sleep 1; echo "$PASSWORD"
    sleep 1; echo "enable"
    sleep 1; echo "show interfaces status"
    sleep 2; echo "exit"
} | nc "$SWITCH" 23 | tr -d '\r'
```

The same pattern runs any configuration: send `configure terminal`, the
commands, `end` and `write memory`.

## Firmware update

Put the image (`output/rtl-swos.bin`, a full 512 KB image) on the TFTP server
and, from a privileged session:

```
switch# copy tftp flash 192.168.0.10 rtl-swos.bin
```

The switch stages the image in flash, verifies its CRC and reboots to apply
it. Progress and errors are printed on the serial console; `show tftp` reports
the state of the last transfer from any session. Wait for the switch to
answer again before the next step.

## Startup config

```
switch# copy startup-config tftp 192.168.0.10 switch.cfg
switch# copy tftp startup-config 192.168.0.10 switch.cfg
switch# reload
```

The first command uploads the startup config; the server must allow the file
to be created. The second replaces the startup config in flash; it takes
effect on the next boot, hence the `reload`. The file uses the same syntax as
`show running-config`. An old config in the flat command syntax can be
converted with `tools/convert-legacy-config.py` first.
