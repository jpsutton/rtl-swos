# VLAN

The RTL827x provides support for up to 4096 802.1Q VLANs, each port can be
assigend a PVID.

## VLAN control
VLANs are controlled by the VLAN table. Configuration of entries is done as
for the L2 entries, apart from that the ASIC does not add entries by itself.

Adding a VLAN entry is done by setting:

```
RTL837x_TBL_DATA_IN_A = 02 0v vv vv
```
where 0x02 designates a valid entry and vvvvv is a 20bit field made of lower 10bits designating whether a
port is a member of a VLAN. The higher 10 bits are '0' for tagged ports
and '1' for untagged ports (the bit-logic definition is `v = (~members) ^ tagged ^
members`). Ports are numbered 0-9, 9 being the CPU-port. Note that for
the RTL8372 devices, the ports are not in their physical order.

Once RTL837x_TBL_DATA_IN_A is set, the entry is added to the table by:

```
RTL837X_TBL_CTRL = 0V VV TT CC
CC: TBL_WRITE | TBL_EXECUTE
VVV: VLAN-Id
TT: 0x02 (TBL_VLAN)
CC: 0x03 (TBL_WRITE | TBL_EXECUTE)
```
The ASIC will clear bit 0 once the entry has been added.

An entry is deleted by adding an invalid entry (00 instead of 0x02 in
RTL837x_TBL_DATA_IN_A).

A port is assigned a PVID by setting the PVID-bits of the corresponding
register of the port. 2 ports share a register. An odd port uses bits [23:12],
an even port uses bits [11:0].  The base register is
RTL837x_PVID_BASE_REG (0x4e1c) and the registers go to 0x4e2c so that also
the CPU-Port may have a PVID.

Register RTL837x_REG_INGRESS (0x4e10) allows to define the ingress rules of
a port. 2 bits define a rule and bits 0-19 are being used. A value of 00
defines no filtering, 01 (0x01) allows only tagged packets, while 10 (0x02)
allows only untagged packets to enter a port.

Register RTL837X_VLAN_PORT_IGR_FLTR (0x4e18) enables or disables ingres VLAN
filtering, each bit corresponds to given port (port0 -> bit0, port9 -> bit9).
When enabled, incomming package's vlan tag is checked against VLAN membership
on given port. When package contains VLAN not in member list, package is dropped.

The default PVID on all port is 1, ingress VLAN filtering is enabled and all types of
frames are accepted on input on all ports.

By default, the ports transmit Ethernet frames with Realtek's proprietary
tag format. By setting bit 6 (0x40) of the respective port configuration
registers 0x1238, 0x1338, ...

## VLAN API
The code currently provides the following functions:
```
void port_pvid_set(uint8_t port, __xdata uint16_t pvid) __banked;
uint16_t port_pvid_get(uint8_t port) __banked;
void vlan_create(void) __banked;   // reads from global vlan_settings
int8_t vlan_get(register uint16_t vlan) __banked;  // returns data in sfr_data
void vlan_delete(uint16_t vlan) __banked;

```

# VLAN configuration on the CLI
VLANs are configured per port in configuration mode, on the serial console or
over telnet. A port is either an access port (an untagged member of one VLAN)
or a trunk (a tagged member of the VLANs in its allowed list, plus an untagged
native VLAN). The firmware derives the member/tagged masks of the VLAN table,
the PVIDs and the ingress acceptance from that per-port state:

* an access port gets its access VLAN as PVID and admits untagged frames only;
* a trunk port gets its native VLAN as PVID and admits tagged and untagged
  frames.

A port that admits tagged frames only, which the ingress register supports,
cannot be configured from the CLI.

```
switch# configure terminal
switch(config)# vlan 10
switch(config-vlan)# name home
switch(config-vlan)# exit
switch(config)# interface ethernet 1/1-8
switch(config-if-range)# switchport access vlan 10
switch(config-if-range)# exit
switch(config)# interface ethernet 1/9
switch(config-if)# switchport mode trunk
switch(config-if)# switchport trunk allowed vlan 10,20-30
switch(config-if)# end
switch# write memory
```

`switchport access vlan N` creates VLAN N if it does not exist yet. The
allowed list of a trunk accepts `all` (the default), `none`, a list such as
`10,20-30`, or `add LIST` / `remove LIST`; at most 8 ranges are kept.
`no vlan N` deletes a VLAN; VLAN 1 cannot be deleted. `no switchport mode`
returns a port to access mode.

The management interface of the switch (telnet, TFTP, syslog, DHCP) lives in
one VLAN, selected by the VLAN interface that carries its address:

```
switch(config)# interface vlan 10
switch(config-if)# ip address 192.168.0.25 255.255.254.0
switch(config-if)# exit
switch(config)# ip default-gateway 192.168.0.1
```

`ip address dhcp` obtains the address with DHCP instead. Moving the
management interface to a VLAN that no port of your workstation reaches locks
out telnet; recovery then requires the serial console. The management
interface is always bound to one VLAN; the old `vlan 0 mgmt`, which lifted the
restriction, has no equivalent.

The state is shown with:

```
show vlan brief          # VLANs and their access ports
show interfaces trunk    # trunk ports, native VLAN and allowed list
show interfaces status   # per port: access VLAN, "trunk" or port-channel
show running-config
```

A startup config written in the old flat syntax (`vlan 10 home 2 3 9t`,
`pvid 1 20`, `ingress 1a 2u`, `vlan 10 mgmt`) can be converted with
`tools/convert-legacy-config.py`, see `doc/examples/legacy-lab.cfg` and
`doc/examples/lab-converted.cfg`. The old model (member lists per VLAN plus
a PVID and ingress mode per port) does not always map exactly onto access and
trunk ports; the converter notes every such case in a `!` comment.
