# Spanning Tree (STP / RSTP)

The switch can take part in a spanning tree (IEEE 802.1D / 802.1w) so that
redundant links between bridges are blocked instead of forming a loop. The
implementation elects a root bridge from the BPDUs it receives, promotes ports
to forwarding once their listen period expires, ages the root out when it goes
silent, and blocks a port on which it sees its own BPDU.

STP is enabled and controlled from the CLI (serial console or telnet), as
follows:

## Quick start

```
switch(config)# feature spanning-tree        # start participating
switch(config)# no feature spanning-tree     # stop, all ports back to forwarding
```

Live status is shown by `show spanning-tree`.

With no other bridge around, the switch elects itself root and every port ends
up forwarding — you can leave it on safely. `write memory` saves the settings
to the startup config so that they survive a reboot; there they appear as:

```
interface ethernet 1/1
 spanning-tree portfast
!
spanning-tree priority 61440
feature spanning-tree
```

## Hardware background

BPDUs are addressed to `01:80:C2:00:00:00`, a reserved link-local group. The
ASIC's Reserved-Multicast action for that address decides what happens to the
frame.

Forwarding to the CPU port works normally: the 8051 sits behind an ordinary
port of the internal switch and is an ordinary member of a forwarding mask.
The *trap* action does not deliver to it. Its destination is an external CPU
attached to a physical port (`cpuTag_externalCpuPort_set`, `EXT_CPU_CTRL` in
the vendor SDK), which these boards do not populate. The ACL trap and
redirect actions do not deliver to the 8051 either.

Delivery therefore uses the *forward* action, constrained to the CPU port
by a static L2 multicast entry (`port_l2mc_set()`), one per VLAN in use:

* while STP runs, the entry's member mask is the CPU port only — BPDUs reach
  the CPU and are not flooded to other ports, as a participating bridge
  requires;
* with STP off, the same entries are retargeted to all ports, restoring the
  transparency an unmanaged switch is expected to have, so a surrounding
  spanning tree can span *through* this device.

A BPDU delivered this way is an ordinary frame to the port's ingress logic
and passes through its acceptable-frame-type filter. BPDUs are untagged, so a
port set to admit tagged frames only never delivers one to the CPU.
`stp_setup()` prints a warning for every STP-enabled port in that state. The
CLI never puts a port in that state: access ports admit untagged frames, trunk
ports admit both.

## Link aggregation

A LAG (Link Aggregation Group) is handled like an additional port by the STP
protocol, with its own timers and states. The switch devices support up to 4
LAGs, which are shown alongside the physical ports in the STP status. A group
carries its own path cost, priority, edge and guard settings, and gets its own
row in `show spanning-tree`.

```
switch(config)# interface port-channel 1
switch(config-if)# spanning-tree cost 10000          # the group decides, not its members
switch(config-if)# spanning-tree portfast disable
```

LAGs are configured with `channel-group`, see [Link Aggregation](link_aggregation.md).
Once a port is a member of a LAG it can no longer be configured individually
for STP: a `spanning-tree` command under a member's `interface ethernet` is
refused with a message naming the port-channel to configure instead.
Membership is re-read from the aggregation registers once a second, so a
change of membership is picked up without any coordination between the two.

The switch hardware does not handle STP state for a LAG as a whole. State
changes for the members have to be done by the firmware, updating every member
port, which is possible using a single register write. BPDUs go out through the
lowest member and carry the group's own port id. Losing one member of a live
LAG is not a topology change; the logical port only goes down with its last
link.

Port states live in `RTL837X_MSTP_STATES (0x5310)`, two bits per port:
`00` disabled, `01` blocking, `10` learning, `11` forwarding. A port in
blocking forwards nothing between ports, but it still sends what the CPU
hands it and still passes a received BPDU up to the CPU, which is what lets
loop detection go on working on a port it has already blocked.

## Timers

`stp_timers()` runs at 50 Hz (the main loop idles on the 200 Hz system tick and
STP is called every fourth pass), which is what `STP_HZ` in `rtl837x_stp.h`
encodes. All configured values are in seconds:

| setting | default | range |
|---|---|---|
| `spanning-tree hello-time <n>` | 2 | 1–10 |
| `spanning-tree max-age <n>` | 20 | 6–40 |
| `spanning-tree forward-time <n>` | 15 | 4–30 |
| `spanning-tree transmit hold-count <n>` | 6 | 1–10 |

These are global configuration commands; the `no` form restores the default.
A port entering the tree spends `forward-time` seconds in blocking before it
forwards (an edge port skips the wait). Root information is discarded after
`max-age` seconds without a BPDU, and the switch then reclaims the root role.

## Topology changes

A change on a local non-edge port (the link coming or going, a port promoted
to forwarding) flushes the addresses learned on it and sets the TC flag in
our BPDUs for `max-age + forward-time` seconds. A TC flag received in a BPDU is passed
on: the switch flushes the other non-edge ports once and keeps the flag in
its own BPDUs until one hello after the last flagged frame, so the
notification crosses the switch instead of dying at it. A legacy TCN is
acknowledged with TCA and then treated like a local change.

## Bridge settings

In global configuration mode:

```
spanning-tree priority <0-61440>    # a multiple of 4096, default 32768
spanning-tree mode rstp|stp         # RST BPDUs (default) or legacy Config BPDUs
spanning-tree hello-time|max-age|forward-time <seconds>
spanning-tree transmit hold-count <n>
```

The bridge with the lowest priority wins the root election; ties are broken by
the MAC address. If you do not want this switch to become the root of an
existing network, give it a worse priority than the current root —
`spanning-tree priority 61440` is the usual "never me" value.

## Per-port settings

Under `interface ethernet 1/<N>` (or `interface port-channel <N>` for a LAG):

```
spanning-tree portfast                 # edge port
spanning-tree portfast disable         # never an edge port
no spanning-tree portfast              # automatic edge detection (default)
spanning-tree cost <1-200000000>       # path cost; no form = automatic (by speed)
spanning-tree port-priority <0-240>    # port priority, steps of 16, default 128
spanning-tree bpduguard enable
spanning-tree guard root
spanning-tree bpdufilter enable        # neither send nor accept BPDUs
spanning-tree link-type point-to-point|shared   # no form = automatic
```

Every port takes part in STP while it runs; excluding a single port from the
protocol (the old `stp port <n> off`) is not reachable from the CLI.

**portfast** — an edge port forwards immediately and does not trigger a
topology change when its link comes and goes; by default a port is promoted to
edge after three seconds without a BPDU, and demoted as soon as one arrives. Use
`spanning-tree portfast` for ports where only hosts are attached.

**bpduguard / guard root** — BPDU guard disables a port as soon as a BPDU
arrives on it (a host port should never see one); root guard keeps a port from
ever becoming the path to the root, which protects an existing topology from a
newly attached bridge that claims a better priority.

**bpdufilter** — the port neither sends nor accepts BPDUs. Useful when the device
on the far side reacts badly to them (some unmanaged switches with loop
prevention cut the link) but you still want STP on the rest of the ports.

## Status

`show spanning-tree` shows the protocol version, the bridge, the elected root
(priority and MAC), the path cost to it, the root port, the topology-change
counter and, per port and LAG, the live state read from the ASIC, the role and
the edge status.

## Limitations

* One spanning-tree instance; no MSTP, no per-VLAN trees.
* No proposal/agreement handshake — an RST-capable neighbour will still
  converge, but through the timers rather than the fast transition.
* Port roles are approximated: the root port and designated ports are
  distinguished, alternate/backup are not.
* Topology changes propagate away from the root only: nothing is announced
  on the root port (no TCN and no BPDUs at all), so bridges upstream rely on
  their own detection.
