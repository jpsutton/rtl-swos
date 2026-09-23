#!/usr/bin/env python3
"""
Convert a startup config written in the old flat command syntax
(`vlan 10 home 2 3 9t`, `pvid 1 20`, `ip 192.168.0.25`, ...) into the
block syntax of the modal CLI.

The old syntax describes VLAN membership per VLAN (member ports, tagged
or not) plus a PVID and an ingress mode per port. The new one describes
each port: access (one untagged VLAN) or trunk (allowed list + native
VLAN). The converter reconstructs that per port and says so in a "!"
comment wherever the old state cannot be expressed exactly, e.g. a port
that was an untagged member of two VLANs.

    tools/convert-legacy-config.py old.cfg > new.cfg

Load the result with `copy tftp startup-config <server> new.cfg` and
reload, after reading the notes at its top.
"""
import argparse
import sys

NPORTS = 9


class Legacy:
    def __init__(self):
        self.vlans = {1: {p: False for p in range(1, NPORTS + 1)}}  # vid -> {port: tagged}
        self.names = {}
        self.pvid = {p: 1 for p in range(1, NPORTS + 1)}
        self.ingress = {}
        self.mgmt_vlan = 1
        self.ip = self.mask = self.gw = None
        self.dhcp = False
        self.hostname = None
        self.passwd = None
        self.telnet = False
        self.telnet_timeout = None
        self.port_name = {}
        self.port_speed = {}
        self.port_off = set()
        self.mtu = {}
        self.eee_off = set()
        self.igmp = False
        self.syslog = None      # (ip, port)
        self.syslog_on = False
        self.lag = {}           # group -> [ports]
        self.laghash = {}
        self.mirror = None      # (dst, [(port, 'r'|'t'|'b')])
        self.bw_in = {}
        self.bw_out = {}
        self.mac = None
        self.notes = []


SPEED = {"10m": "10", "100m": "100", "1g": "1000", "2g5": "2500",
         "5g": "5000", "10g": "10000", "auto": None}


def parse(lines, st):
    for n, raw in enumerate(lines, 1):
        line = raw.strip()
        if not line or line.startswith("!") or line.startswith("#"):
            continue
        w = line.split()
        cmd = w[0]
        try:
            handled = handle(cmd, w, st)
        except (IndexError, ValueError):
            handled = False
        if not handled:
            st.notes.append(f"line {n} not converted: {line}")


def handle(cmd, w, st):
    if cmd == "vlan":
        vid = int(w[1])
        if len(w) == 3 and w[2] == "d":
            st.vlans.pop(vid, None)
            st.names.pop(vid, None)
            return True
        if len(w) == 3 and w[2] == "mgmt":
            st.mgmt_vlan = vid
            return True
        rest = w[2:]
        if rest and rest[0][0].isalpha():
            st.names[vid] = rest[0]
            rest = rest[1:]
        members = {}
        for tok in rest:
            tagged = tok.endswith("t")
            members[int(tok.rstrip("t"))] = tagged
        st.vlans[vid] = members      # the legacy command replaces the membership
        return True
    if cmd == "pvid":
        st.pvid[int(w[1])] = int(w[2])
        return True
    if cmd == "ingress":
        for tok in w[1:]:
            st.ingress[int(tok[:-1])] = tok[-1]
        return True
    if cmd == "ip":
        if w[1] == "dhcp":
            st.dhcp = True
        else:
            st.ip = w[1]
        return True
    if cmd == "netmask":
        st.mask = w[1]
        return True
    if cmd == "gw":
        st.gw = w[1]
        return True
    if cmd == "hostname":
        st.hostname = w[1]
        return True
    if cmd == "passwd":
        st.passwd = w[1]
        return True
    if cmd == "telnet":
        if w[1] == "on":
            st.telnet = True
        elif w[1] == "off":
            st.telnet = False
        elif w[1] == "timeout":
            st.telnet_timeout = int(w[2])
        elif w[1] == "bind":
            st.notes.append(f"`telnet bind {w[2]}` dropped: telnet listens on the management "
                            "address; there is no bind in the new syntax")
        else:
            return False
        return True
    if cmd == "port":
        p = int(w[1])
        if w[2] == "name":
            st.port_name[p] = " ".join(w[3:])
        elif w[2] == "off":
            st.port_off.add(p)
        elif w[2] == "on":
            st.port_off.discard(p)
        elif w[2] in SPEED:
            if SPEED[w[2]]:
                st.port_speed[p] = SPEED[w[2]]
            else:
                st.port_speed.pop(p, None)
        else:
            return False
        return True
    if cmd == "mtu":
        st.mtu[int(w[1])] = int(w[2])
        return True
    if cmd == "eee":
        if w[1] == "off" and len(w) >= 3:
            st.eee_off.add(int(w[2]))
            return True
        if w[1] == "on" and len(w) >= 3:
            st.eee_off.discard(int(w[2]))
            return True
        return False
    if cmd == "igmp":
        st.igmp = w[1] == "on"
        return True
    if cmd == "syslog":
        ip, port = st.syslog or (None, 514)
        if w[1] == "ip":
            ip = w[2]
        elif w[1] == "port":
            port = int(w[2])
        elif w[1] == "on":
            st.syslog_on = True
        elif w[1] == "off":
            st.syslog_on = False
        else:
            return False
        st.syslog = (ip, port)
        return True
    if cmd == "lag":
        g = int(w[1])
        if w[2] == "d":
            st.lag.pop(g, None)
        else:
            st.lag[g] = [int(x) for x in w[2:]]
        return True
    if cmd == "laghash":
        names = {"spa": "src-port", "smac": "src-mac", "dmac": "dst-mac", "sip": "src-ip",
                 "dip": "dst-ip", "sport": "l4-src-port", "dport": "l4-dst-port"}
        st.laghash[int(w[1])] = [names[x] for x in w[2:]]
        return True
    if cmd == "mirror":
        if w[1] == "off":
            st.mirror = None
            return True
        dst = int(w[1])
        srcs = []
        for tok in w[2:]:
            d = tok[-1] if tok[-1] in "rt" else "b"
            srcs.append((int(tok.rstrip("rt")), d))
        st.mirror = (dst, srcs)
        return True
    if cmd == "bw":
        direction, port, val = w[1], int(w[2]), w[3]
        if val == "off":
            (st.bw_in if direction == "in" else st.bw_out).pop(port, None)
        else:
            (st.bw_in if direction == "in" else st.bw_out)[port] = int(val, 16)
        return True
    if cmd == "mac":
        st.mac = w[1]
        return True
    return False


def ranges(vids):
    vids = sorted(vids)
    out, i = [], 0
    while i < len(vids):
        j = i
        while j + 1 < len(vids) and vids[j + 1] == vids[j] + 1:
            j += 1
        out.append(str(vids[i]) if i == j else f"{vids[i]}-{vids[j]}")
        i = j + 1
    return ",".join(out)


def emit(st, out):
    o = []
    for n in st.notes:
        o.append(f"! NOTE: {n}")

    lagof = {p: g for g, ps in st.lag.items() for p in ps}

    ports = {}
    for p in range(1, NPORTS + 1):
        untagged = sorted(v for v, m in st.vlans.items() if m.get(p) is False)
        tagged = sorted(v for v, m in st.vlans.items() if m.get(p) is True)
        pvid = st.pvid[p]
        ing = st.ingress.get(p)
        cfg = []
        if not tagged and ing != "t":
            if len(untagged) > 1:
                o.append(f"! NOTE: port {p} was an untagged member of VLANs "
                         f"{ranges(untagged)}; converted to access VLAN {pvid} (its PVID)")
            access = pvid if (pvid in untagged or not untagged) else untagged[0]
            if access != 1:
                cfg.append(f" switchport access vlan {access}")
        else:
            allowed = sorted(set(untagged) | set(tagged))
            native = pvid
            extra = [v for v in untagged if v != native]
            if extra:
                o.append(f"! NOTE: port {p} carried VLANs {ranges(extra)} untagged besides "
                         f"its PVID {native}; they are tagged on the trunk now")
            if ing == "t" and native in allowed:
                # A trunk accepts tagged frames only when its native VLAN is
                # not allowed; that also ends the port's membership in it.
                allowed.remove(native)
                o.append(f"! NOTE: port {p} accepted tagged frames only; VLAN {native} (its "
                         "PVID) is left off the trunk to keep that, so the port no longer "
                         f"carries VLAN {native}")
            cfg.append(" switchport mode trunk")
            if native != 1:
                cfg.append(f" switchport trunk native vlan {native}")
            cfg.append(f" switchport trunk allowed vlan {ranges(allowed) if allowed else 'none'}")
        ports[p] = cfg

    if st.hostname:
        o += ["!", f"hostname {st.hostname}"]
    o.append("!")
    for vid in sorted(st.vlans):
        if vid == 1 and vid not in st.names:
            continue
        o.append(f"vlan {vid}")
        if vid in st.names:
            o.append(f" name {st.names[vid]}")
    if 1 not in st.vlans:
        o.append("! (VLAN 1 was deleted; it cannot be deleted in the new syntax, but no port "
                 "is left in it)")
    o.append("!")
    for g in sorted(st.lag):
        o.append(f"interface port-channel {g}")
        if g in st.laghash:
            o.append(f" load-balance {' '.join(st.laghash[g])}")
        o.append("!")
    for p in range(1, NPORTS + 1):
        o.append(f"interface ethernet 1/{p}")
        if p in st.port_name:
            o.append(f" description {st.port_name[p]}")
        if p in st.port_off:
            o.append(" shutdown")
        if p in st.port_speed:
            o.append(f" speed {st.port_speed[p]}")
        if p in st.mtu:
            o.append(f" mtu {st.mtu[p]}")
        o += ports[p]
        if p in st.eee_off:
            o.append(" no power efficient-ethernet")
        if p in st.bw_in:
            o.append(f" rate-limit input {st.bw_in[p]}")
        if p in st.bw_out:
            o.append(f" rate-limit output {st.bw_out[p]}")
        if p in lagof:
            o.append(f" channel-group {lagof[p]} mode on")
        o.append("!")
    o.append(f"interface vlan {st.mgmt_vlan}")
    if st.dhcp:
        o.append(" ip address dhcp")
    elif st.ip:
        o.append(f" ip address {st.ip} {st.mask or '255.255.255.0'}")
    if st.mac:
        o.append(f" mac-address {st.mac}")
    o.append("!")
    if st.gw and not st.dhcp:
        o.append(f"ip default-gateway {st.gw}")
    if st.igmp:
        o.append("ip igmp snooping")
    if st.syslog_on and st.syslog and st.syslog[0]:
        ip, port = st.syslog
        o.append(f"logging host {ip}" + (f" port {port}" if port != 514 else ""))
    if st.mirror:
        dst, srcs = st.mirror
        for p, d in srcs:
            o.append(f"monitor session 1 source interface ethernet 1/{p}"
                     + {"r": " rx", "t": " tx", "b": ""}[d])
        o.append(f"monitor session 1 destination interface ethernet 1/{dst}")
    if st.telnet:
        o.append("feature telnet")
    if st.telnet_timeout or st.passwd:
        o.append("!")
        o.append("line vty")
        if st.telnet_timeout:
            o.append(f" exec-timeout {st.telnet_timeout // 60} {st.telnet_timeout % 60}")
        if st.passwd:
            o.append(f" password {st.passwd}")
    o += ["!", "end"]
    out.write("\n".join(o) + "\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("legacy", help="config in the old flat syntax")
    args = ap.parse_args()
    st = Legacy()
    with open(args.legacy) as f:
        parse(f.read().splitlines(), st)
    emit(st, sys.stdout)


if __name__ == "__main__":
    main()
