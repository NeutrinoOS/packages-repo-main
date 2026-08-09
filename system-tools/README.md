# system-tools

Neutrino userspace package containing:

- `dmesg`, `insmod`, and `lsmod`
- `lsdisk`, `lspci`, and `sensors`
- `mkneufs`, `mkpart`, and `mount`
- `shutdown`, `top`, and `userctl`
- `tree`

Build with:

```sh
make package
```

The neupak archive is written to `out/system-tools.zip`.

`tree` recursively lists the current directory by default. It accepts `-a` to
include hidden entries, `-d` to show directories only, and `-L depth` to limit
recursion.
