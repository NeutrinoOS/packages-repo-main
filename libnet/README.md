# libnet

Shared Neutrino library for networking and HTTP. Applications link
`libnet.so.0` and depend on the `libnet` package rather than a particular
`networkd` or `tcpd` binary.

Installed payload:

```text
/include/neutrino/net.hpp
/include/neutrino/http.hpp
/library/libnet.so.0
/library/libnet.so
```

`net.hpp` is the userspace interface to the Neutrino Network and TCP service
ABIs (connect, listen, accept, read, write, DNS, DHCP). `http.hpp` is protocol
machinery plus HTTP client (`get`, `post`, `request`) and server helpers
(request parsing and response serialization). Neither header names a daemon.

Build with:

```sh
make
make package
```
