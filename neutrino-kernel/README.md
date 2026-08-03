# Neutrino kernel package

This package consumes the kernel produced by the sibling `neutrino` source tree
and creates the archive published through neupak-server.

Build or refresh the package after a kernel change with:

```sh
make package
```

This rebuilds `../../neutrino/out/kernel.elf` when needed and writes
`out/neutrino-kernel.zip`. Bump `package/manifest.toml` according to semantic
versioning before publishing every kernel update. The package build embeds that
manifest version in the kernel; the running release is available through
`uname -r` and is included in `uname -a`.
