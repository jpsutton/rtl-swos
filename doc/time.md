# Time, DNS and TOTP

The switch has no battery-backed clock: it counts seconds from power on and
takes the date and time from an NTP server once the network is up. Host names
are resolved by a small DNS resolver, and the time lets telnet ask for a TOTP
code as a second factor. The resolver and the NTP client come from
RTLPlayground (PRs #446 and #447, by d00f).

## DNS

```
switch(config)# ip name-server 192.168.0.1 9.9.9.9
switch# nslookup pool.ntp.org
Looking up; the result will be in show hosts
switch# show hosts
Name servers: 192.168.0.1 9.9.9.9
Last lookup:  pool.ntp.org is 162.159.200.123
```

One or two servers; without any, the resolver uses the server DHCP handed out.
Only IPv4 address records are looked up. A query goes to the servers in turn
every two seconds and gives up after six tries; CNAME records and compressed
names in the answer are understood. A name that already is a dotted address
is taken without a query. `nslookup` returns at once: the answer arrives
asynchronously and `show hosts` reports it.

## NTP

```
switch(config)# ntp server pool.ntp.org
switch(config)# clock timezone CET 1
switch(config)# clock summer-time CEST recurring eu
switch# show clock
14:02:37 CEST Tue Sep 23 2026
switch# show ntp
NTP server: pool.ntp.org (162.159.200.123)
Poll interval: 60 min
Time zone: CET +01:00, summer time CEST (EU rule)
Status: synchronised, stratum 3, last update 12 min ago
```

The client speaks SNTP version 4. It synchronises as soon as a server is
configured, then every 60 minutes; the server name is resolved before every
synchronisation. An answer from an unsynchronised server is ignored, and a
failed attempt is repeated after 30 seconds. Until the first synchronisation
`show clock` says the time is not set. The time is lost at every restart.

`clock timezone NAME HOURS [MINUTES]` sets the offset from UTC, -12 to +14
hours, with 0, 30 or 45 minutes. `clock summer-time NAME recurring` adds an
hour while daylight saving time applies:

* `us` (the default, as for IOS's plain `recurring`): from 02:00 local time
  on the second Sunday of March to 02:00 on the first Sunday of November;
* `eu`: from 01:00 UTC on the last Sunday of March to 01:00 UTC on the last
  Sunday of October.

## TOTP second factor for telnet

```
switch(config)# line vty
switch(config-line)# totp secret JBSWY3DPEHPK3PXPJBSWY3DPEHPK3PXP
Authenticator URI: otpauth://totp/rtl-swos-94830a?secret=JBSWY3DPEHPK3PXPJBSWY3DPEHPK3PXP&issuer=rtl-swos
switch(config-line)# login totp
switch(config-line)# end
switch# show totp
```

The secret is a base32 string decoding to 10 to 32 bytes (16 to 51
characters); enter it, or the URI, in an authenticator app. With `login totp`
a telnet login asks for the 6-digit code after the password. Codes are
RFC 6238 TOTP with HMAC-SHA1 and a 30 second step; one step either way is
accepted.

The check fails closed: while NTP has not set the clock, telnet logins with
`login totp` are refused, and entering `login totp` without a clock warns
about that. The serial console does not ask for a code, so it stays the way
in. `show totp` (privileged) shows the state and the current code, for
comparing with the app.

The secret is stored in the clear in the running and startup configuration,
like the telnet password.
