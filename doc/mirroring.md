# Mirroring

The RTL827x provides support to mirror packages from a number of ports to a mirroring
ports. For each mirrored port it is possible to define whether received, transmitted
or both types of packages are being mirrored.

## Mirroring control
Mirroring is enabled by setting RTL837x_MIRROR_CTRL(0x6048):

```
RTL837x_MIRROR_CTRL = port << 1 | 0x1
```
Ports are numbered 0-9, 9 being the CPU-port.
Mirroring is stopped by writing 0 to this register.

The mirrored ports are configured in RTL837x_MIRROR_CONF(0x604c):

```
RTL837x_MIRROR_CONF = RRRR TTTT
RRRR: 16 bit mask for ports where received packets are mirrored
TTTT: 16 bit mask for ports where transmitted packets are mirrored
```

## Mirroring API
The code currently provides the following functions:
```
void port_mirror_set(register uint8_t port, __xdata uint16_t rx_pmask, __xdata uint16_t tx_pmask) __banked
void port_mirror_del(void)
```

# Mirroring on the CLI
Mirroring is configured as SPAN session 1 in global configuration mode. There
is one session, with one destination port and any number of source ports:
```
monitor session 1 source interface ethernet 1/<N> [rx|tx|both]
monitor session 1 destination interface ethernet 1/<N>
no monitor session 1 source interface ethernet 1/<N>
no monitor session 1 destination
no monitor session 1
```
A source mirrors received (`rx`), transmitted (`tx`) or, by default, both
kinds of packets. The destination cannot also be a source. `no monitor
session 1` deletes the whole configuration.

To mirror what port 1 receives and everything port 2 sends and receives to
port 4:
```
switch(config)# monitor session 1 source interface ethernet 1/1 rx
switch(config)# monitor session 1 source interface ethernet 1/2
switch(config)# monitor session 1 destination interface ethernet 1/4
switch(config)# end
switch# show monitor session 1
Session 1 (active)
  Source rx:    Eth1/1 Eth1/2
  Source tx:    Eth1/2
  Destination:  Eth1/4
```
