# Change Log

## [rtl-swos 0.1.0] - unreleased

First release of the rtl-swos fork.

## Added

- Modal command line in the industry-standard style (user/privileged EXEC,
  global configuration, interface, port-channel, VLAN, management interface
  and line submodes) with `?` help, Tab completion, abbreviations, `no`
  forms and `^` error markers; EXEC commands work in configuration modes.
  See doc/cli.md.
- Block-structured running configuration, `write memory`, `show
  running-config` / `startup-config`, replayed at boot with per-line errors.
- Show suite: interfaces status/counters/trunk/transceiver, vlan, mac
  address-table, spanning-tree, port-channel, ip interface brief, ip igmp
  snooping, monitor, logging, tftp, version, history.
- Interface ranges: `interface ethernet 1/1-4,1/7`, `interface range ...`.
- LACP: `channel-group N mode active|passive`, `lacp rate|port-priority|
  system-priority|min-links`, `show lacp`; IOS `port-channel load-balance`.
- `spanning-tree bpdufilter enable` (alias `spanning-tree disable`) takes a
  port out of spanning tree; `ip igmp snooping mrouter`; trunks without their
  native VLAN accept tagged frames only.
- Default configuration: DHCP on VLAN 1, telnet on; `make CONFIG=file`.
- DNS resolver (`ip name-server`, `show hosts`, `nslookup`) and SNTP client
  (`ntp server`, `clock timezone`, `clock summer-time`, `show clock`,
  `show ntp`), from RTLPlayground #446 and #447.
- TOTP second factor for telnet logins (`totp secret`, `login totp`).
- `ping HOST [repeat N] [size N]`; `show interfaces ethernet LIST` detail.
- LLDP (`feature lldp`, `show lldp neighbors [detail]`); local event log
  (`show logging`, `clear logging`); a fourth code bank.
- Telnet command history (arrow keys, ^P/^N); `copy startup-config
  running-config`; `show running-config interface ...|vlan ...`.
- In-band firmware update and configuration transfer over TFTP.
- Separate CLI sessions for the serial console and telnet.
- tools/convert-legacy-config.py converts RTLPlayground configurations.

## Breaking changes

- The web interface and its HTTP server are removed.
- The flat console command language and its configuration syntax are
  removed; convert old configurations with tools/convert-legacy-config.py.
- Images are named rtl-swos-*.bin.

# RTLPlayground history

The entries below predate the fork and describe RTLPlayground.

## [0.x] - 2026-xx-XX

## Added

- Web UI
  - Compress the embedded assets (minify + gzip) and switch to a single-page layout. #315
  - Replace the multi-page UI with a themed single-page app: light, dark and Selenized themes following the browser by default,
    English, Japanese and Chinese, save to flash merges the command log into the startup config and verifies the write,
    firmware images are checked in the browser before upload. #429

## Changed

## Fixed

## Breaking changes

- Config
  - VLAN don't accept port `u`-suffix anymore.
    So `vlan 1 4u` is not valid anymore.
    Replace it with `vlan 1 4`.
- Commands
  - port zero/`0` is treated as the `CPU_PORT`. #326
  - Many commands don't accept the CPU_PORT anymore. See #334.
    When the CPU_PORT is needed, the command/service will add the CPU_PORT automaticly.
    Only `isolate` accept the CPU_PORT as destination port.

## [v0.1_aplha] - 2025-11-03

First release.