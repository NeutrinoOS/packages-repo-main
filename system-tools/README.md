# system-tools

Neutrino userspace package containing:

- `dmesg`, `insmod`, and `lsmod`
- `lscpu`, `lsdisk`, `lspci`, and `sensors`
- `mkneufs`, `mkpart`, and `mount`
- `shutdown`, `top`, and `userctl`
- `tree`

Build with:

```sh
make package
```

The neupak archive is written to `out/system-tools.zip`.

`lscpu` reports the x86_64 CPU identity and the CPUID feature flags exposed by
the kernel's extensible system-monitor descriptor stream.

`tree` recursively lists the current directory by default. It accepts `-a` to
include hidden entries, `-d` to show directories only, and `-L depth` to limit
recursion.

`userctl caps` lists the kernel capability names and bit numbers understood by
the account tool. Account creation accepts a decimal mask, `all`, the
least-privilege `desktop` profile, or a comma-separated capability list:

```sh
userctl create alice desktop
userctl create operator process-spawn,system-monitor,kernel-log
```

The desktop profile grants process spawning, graphics, input, audio, and
network endpoints. It deliberately excludes raw storage, mounting,
system administration, process inspection/control, and ACL override.
