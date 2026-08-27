# network-tools

Neutrino userspace package containing:

- `browse`
- `netctl`
- `netget`
- `netshell`
- `ping`
- `tcpecho`

Build with:

```sh
make package
```

The neupak archive is written to `out/network-tools.zip`.

`browse` verifies HTTPS certificate chains with BearSSL and the shared trust
store installed by the `ca-certificates` package; both are package dependencies.

`netctl set-ip` applies a static IPv4 configuration and persists it in the
machine settings hive using the interface MAC address. It must run with machine
settings write access; otherwise the command reports that the live change could
not be saved for the next boot.
