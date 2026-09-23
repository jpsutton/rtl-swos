# Migrating from RTLPlayground

rtl-swos replaces RTLPlayground's web interface and flat console commands
with a modal CLI (see [CLI](cli.md)), so an RTLPlayground startup
configuration does not load as it is: every line would be rejected.

`tools/convert-legacy-config.py` rewrites one:

```
tools/convert-legacy-config.py old.cfg > new.cfg
```

The old syntax describes each VLAN's members plus a PVID and an ingress mode
per port; the new one describes each port as an access port or a trunk. The
converter reconstructs that per port and writes a `! NOTE:` comment at the top
wherever the old state has no exact equivalent, for example a port that was
an untagged member of two VLANs. Read those notes before loading the result.

Check the result on the host before it goes near a switch:

```
make -C test
test/build/test_replay new.cfg
```

`test_replay` runs the file through the real CLI, as the switch does at boot,
and fails on any error line. Then load it with `copy tftp startup-config` and
`reload`, or build it into an image with `make CONFIG=new.cfg`.

`doc/examples/legacy-lab.cfg` and `doc/examples/lab-converted.cfg` are a real
configuration and its conversion.

Installing rtl-swos over RTLPlayground through RTLPlayground's own web
firmware upload has not been tested; flashing the chip always works (see
[Installing](installing.md)).
