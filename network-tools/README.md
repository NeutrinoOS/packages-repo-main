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
