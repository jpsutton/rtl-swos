# Command line reference

rtl-swos is configured through a modal command line in the industry-standard
style: user EXEC, privileged EXEC, global configuration and per-object
submodes. The same CLI runs on the serial console (115200 8N1) and, with
`feature telnet`, over telnet. Each has its own session: mode, submode and
privilege level are not shared.

```
rtl-swos-94830a> enable
rtl-swos-94830a# configure terminal
Enter configuration commands, one per line. End with 'end'.
rtl-swos-94830a(config)# interface ethernet 1/3
rtl-swos-94830a(config-if)# description printer
rtl-swos-94830a(config-if)# switchport access vlan 20
rtl-swos-94830a(config-if)# end
rtl-swos-94830a# write memory
```

## Editing and help

- Every keyword can be abbreviated to a unique prefix: `conf t`, `sh int st`,
  `wr`. An ambiguous prefix (`sp` in interface mode: `speed` or
  `spanning-tree`) is reported as `% Ambiguous command`.
- `?` lists what may follow at the cursor, with a short description;
  after a partial word it lists the matching keywords. Tab completes a
  unique prefix.
- An unknown or malformed word is marked:

  ```
  rtl-swos-94830a# show vlan brieff
                             ^
  % Invalid input detected at '^' marker.
  ```

- `no <command>` removes a setting or returns it to its default.
- EXEC commands (`show`, `write`, `copy`, `reload`, ...) work in every
  configuration mode without a `do` prefix.
- A global configuration command typed in a submode runs in global
  configuration mode and leaves the submode, so a pasted configuration
  needs no `exit` lines.
- The up and down arrow keys (or ^P/^N on telnet) recall earlier lines;
  `show history` lists them. The serial console and each telnet session
  have their own history.
- Lines starting with `!` are comments.

## Modes

| Prompt | Mode | Entered with |
|---|---|---|
| `host>` | user EXEC | login |
| `host#` | privileged EXEC | `enable` |
| `host(config)#` | global configuration | `configure terminal` |
| `host(config-if)#` | ethernet interface | `interface ethernet 1/N` |
| `host(config-if-range)#` | several ethernet interfaces | `interface ethernet 1/1-4,1/7` |
| `host(config-if)#` | port-channel | `interface port-channel N` |
| `host(config-if)#` | management interface | `interface vlan N` |
| `host(config-vlan)#` | VLAN | `vlan N` |
| `host(config-line)#` | telnet line | `line vty` |

`exit` goes up one level, `end` returns to privileged EXEC. `disable` drops
back to user EXEC.

## Interface names

Front-panel ports are `ethernet 1/1` to `ethernet 1/9` (the numbering printed
on the case; the firmware maps them to the chip's logical ports per board).
`ethernet` may be abbreviated or left out: `interface e1/5`,
`interface 1/5` and `interface 5` are the same port. Show commands print
`Eth1/N`.

A port list selects several ports at once, as one word:
`interface ethernet 1/1-4,1/7` (NX-OS form) or
`interface range ethernet 1/1-4,1/7` (IOS form). Each item may carry the
`ethernet` prefix (`e1/1-4,e1/7`). A command entered in the range submode
runs once for each port, in port order.

## EXEC commands

| Command | |
|---|---|
| `show running-config` | Current configuration (non-default settings) |
| `show running-config interface [ethernet LIST\|port-channel N\|vlan N]` | Only those interface blocks, e.g. `show run int eth1/3` |
| `show running-config vlan [N]` | Only the VLAN blocks |
| `show startup-config` | The configuration replayed at boot |
| `show version` | Software, build, board, flash size, MAC, uptime |
| `show interfaces [status]` | Link, VLAN, speed and type per port |
| `show interfaces status err-disabled` | Ports disabled by BPDU guard, and the recovery setting |
| `show interfaces [ethernet] LIST` | Detail per port, e.g. `show int eth1/3`: link, speed, MTU, VLANs, port-channel, EEE, rate limits, STP, counters |
| `show interfaces counters` | Packet and error counters |
| `show interfaces trunk` | Trunk ports, native and allowed VLANs |
| `show interfaces transceiver` | SFP modules and their diagnostics (DDM) |
| `show vlan [brief]` | VLAN database with member ports |
| `show mac address-table` | Learned and static MAC addresses |
| `show spanning-tree` | Bridge and port STP state |
| `show port-channel [summary]` | Port-channels, protocol, members, hash fields |
| `show lacp [neighbor]` | LACP ports: state, partner, counters |
| `show lldp neighbors [detail]` | Devices heard on each port (LLDP) |
| `show ip interface brief` | Management address, mask, method, gateway |
| `show ip igmp snooping` | IGMP snooping state |
| `show monitor [session 1]` | Port mirroring |
| `show logging` | Remote syslog and the local event log (links, logins, STP, LACP, NTP, DHCP, saves) |
| `clear logging` | Empty the local event log |
| `show tftp` | Progress or result of the last TFTP transfer |
| `show history` | Command history of this session |
| `show clock` | Local date and time (from NTP) |
| `show ntp [status]` | NTP server and synchronisation |
| `show hosts` | Name servers and the last lookup |
| `show totp` | TOTP login state and the current code (privileged) |
| `nslookup NAME` | Resolve a name; the result appears in `show hosts` |
| `ping HOST [repeat N] [size N]` | ICMP echo, IOS-style `!`/`.` per echo and a summary; defaults 5 echos of 100 bytes, 2 s timeout; any key aborts |
| `write [memory]`, `copy running-config startup-config` | Save the configuration to flash |
| `copy startup-config running-config` | Merge the startup configuration into the running one |
| `copy tftp flash A.B.C.D FILE` | Download a firmware image; it is applied by reloading |
| `copy tftp startup-config A.B.C.D FILE` | Replace the startup configuration (takes effect at reload) |
| `copy startup-config tftp A.B.C.D FILE` | Upload the startup configuration |
| `clear mac address-table dynamic` | Flush learned addresses |
| `clear counters [interface [ethernet] LIST]` | Count interface counters from now on |
| `reload` | Restart |
| `debug ...` | Raw register, SerDes, PHY, XRAM, GPIO and flash access; see `debug ?` |

`config` is accepted in place of `startup-config` in the `copy` commands.
Everything except `show`, `enable` and `exit` needs privileged EXEC.

## Global configuration

| Command | Default |
|---|---|
| `hostname WORD` | `rtl-swos-XXXXXX` from the MAC |
| `vlan N` (1-4094), `no vlan N` | VLAN 1 only; VLAN 1 cannot be deleted |
| `interface ethernet 1/N`, `interface port-channel N` (1-4), `interface vlan N` | |
| `ip default-gateway A.B.C.D` | none |
| `ip name-server A.B.C.D [A.B.C.D]` | the DNS server from DHCP |
| `ntp server HOST` | none (no time) |
| `clock timezone NAME HOURS [MINUTES]` | UTC 0 0 |
| `clock summer-time NAME recurring [eu\|us]` | none; plain `recurring` is the US rule |
| `ip igmp snooping` | off |
| `logging host A.B.C.D [port N]` | off, port 514 |
| `monitor session 1 source interface ethernet 1/N [rx\|tx\|both]` | |
| `monitor session 1 destination interface ethernet 1/N` | |
| `no monitor session 1` | |
| `feature spanning-tree` | off |
| `lacp system-priority N` | 32768 |
| `port-channel load-balance src-mac\|dst-mac\|src-dst-mac\|src-ip\|dst-ip\|src-dst-ip\|src-port\|dst-port\|src-dst-port` | hash of every port-channel |
| `spanning-tree mode rstp\|stp` | rstp |
| `spanning-tree priority N` | 32768 |
| `spanning-tree hello-time N`, `forward-time N`, `max-age N`, `transmit hold-count N` | 2, 15, 20, 6 |
| `feature telnet` | off |
| `errdisable recovery cause bpduguard`, `errdisable recovery interval N` | off, 300 s; `no shutdown` also recovers a port |
| `feature lldp`, `lldp run` | off; LLDP every 30 s, hold time 120 s |
| `line vty` | |

The hardware has one mirror session; several sources may be added to it
one line at a time.

## Ethernet interface

| Command | Default |
|---|---|
| `description LINE` | none |
| `shutdown` | up (not available on SFP ports) |
| `speed auto\|10\|100\|1000\|2500\|5000\|10000` | auto |
| `duplex auto\|full\|half` | auto; half only at 10/100 |
| `mtu 64-16383` | 16383 |
| `power efficient-ethernet auto`, `no power efficient-ethernet` | on |
| `switchport mode access\|trunk` | access |
| `switchport access vlan N` | 1 |
| `switchport trunk native vlan N` | 1 |
| `switchport trunk allowed vlan LIST\|add LIST\|remove LIST\|all\|none` | all |
| `switchport protected` | off |
| `rate-limit input KBPS [drop]`, `rate-limit output KBPS` | none; input sends pause frames unless `drop` |
| `channel-group N [mode on\|active\|passive]` | none; `on` is static, `active`/`passive` run LACP |
| `lacp rate fast\|normal`, `lacp port-priority N` | normal, 32768 |
| `lldp transmit`, `lldp receive` | both on (with `feature lldp`) |
| `spanning-tree portfast [disable]`, `bpduguard enable`, `bpdufilter enable`, `guard root`, `cost N`, `port-priority N`, `link-type point-to-point\|shared` | |
| `spanning-tree bpdufilter enable`, alias `spanning-tree disable` | takes part in STP; the port leaves STP (no BPDUs) and always forwards |
| `ip igmp snooping mrouter` | off; static multicast router port (one mask for all VLANs) |

A VLAN list is `10,20-30` style. Referring to a VLAN that does not exist
creates it, with a note. A trunk whose native VLAN is not in its allowed
list accepts tagged frames only.

`spanning-tree disable` is an alias of `spanning-tree bpdufilter enable`,
shown in the running configuration as the latter. `ip igmp snooping mrouter`
is an extension: the established CLIs set router ports per VLAN, the chip has
one mask.

A port with a `channel-group` takes its spanning-tree settings from its
port-channel.

## Port-channel

`interface port-channel N` holds the settings of a link aggregation group,
static or LACP (see [Link aggregation](link_aggregation.md)):

| Command | Default |
|---|---|
| `lacp min-links N` | 1: bundle LACP ports only when at least N are ready |
| `load-balance FIELD...` | `src-mac dst-mac src-ip dst-ip l4-src-port l4-dst-port` |
| `spanning-tree ...` | as on an ethernet interface |

Fields: `src-port`, `src-mac`, `dst-mac`, `src-ip`, `dst-ip`, `l4-src-port`,
`l4-dst-port`.

## Management interface

The switch has one IP interface. `interface vlan N` selects the management
VLAN:

| Command | |
|---|---|
| `ip address A.B.C.D MASK` | static address |
| `ip address dhcp` | DHCP client |
| `mac-address aabb.ccdd.eeff` | management MAC; `no mac-address` restores the one read at boot |

## VLAN

`vlan N` creates the VLAN and enters its submode; `name WORD` names it (up to
32 characters).

## Telnet line

`line vty` (line numbers, as in `line vty 0 4`, are accepted and ignored; there is one telnet session):

| Command | Default |
|---|---|
| `password WORD` | `1234` |
| `exec-timeout MIN [SEC]` | 10 minutes; at least 30 seconds, `0 0` = never |
| `totp secret BASE32` | none; the shared secret of an authenticator app |
| `login totp` | off; ask for a TOTP code after the password |

See [Time, DNS and TOTP](time.md).

## Startup configuration

`write memory` stores the running configuration in the flash sector at
0x70000. At boot it is replayed line by line in global configuration mode;
a line that fails is printed together with its error and skipped, the rest
of the configuration still applies. The image built by `make` carries
`config.txt` from the source tree (or the file `CONFIG=` names) as its
initial startup configuration; a factory reset restores it. The default
one runs the management interface as a DHCP client on VLAN 1, answering on
192.168.2.2/24 until a lease arrives, with telnet on.

A configuration in the flat command syntax of earlier firmware can be
converted with `tools/convert-legacy-config.py old.cfg > new.cfg`; it
writes a `! NOTE:` line wherever the old state has no exact equivalent.
`test/build/test_replay new.cfg` (built by `make -C test`) replays a file
through the real CLI on the host and fails on any error line.
