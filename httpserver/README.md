# httpserver

A small HTTP/1.0 file server for Neutrino. It serves files beneath the working
directory from which it is started.

```text
httpserver --port 8192
```

The default port is 8000. `GET` and `HEAD` are supported, `/` maps to
`index.html`, and a small set of common content types is returned. Directory
listings, uploads, and parent-directory traversal are intentionally not
supported.

The Neutrino `networkd` and `tcpd` services must already be running (provided
by the `network-services` package). The server binds all configured network
interfaces through `tcpd`, so do not serve a directory containing private
files on an untrusted network.
