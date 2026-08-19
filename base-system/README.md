# Base system

This package provides Neutrino's init process, shell, and service-management
tools. The graphical session is provided separately by the `desktop` package.

`init` is Neutrino's PID 1 and service manager. It loads service definitions
from `@sys/config/services/*.service`, starts enabled services after the root
principal is established, tracks their PIDs and exit status, applies restart
policy, and retains the most recent 8 KiB of combined standard output and
standard error for each service.

Service definitions use one `key=value` setting per line:

```ini
description=Example background service
executable=@sys/binary/example.elf
arguments=--optional arguments
user=root
after=networkd
type=simple
enabled=true
restart=on-failure
```

`executable` is required. The optional `arguments`, `description`, and `after`
settings default to empty; `user` defaults to `root`, `type` defaults to
`simple`, `enabled` defaults to `false`, and `restart` defaults to
`on-failure`. Service type may be `simple` or `oneshot`; a successful oneshot
is reported as `completed`. Restart policy may be `never`, `on-failure`, or
`always`. `after` names one service that must be started first.

Use the `service` program to inspect and control the manager:

```text
service list
service status [name]
service start <name>
service stop <name>
service restart <name>
service logs <name>
```

Requests are authorized with the kernel's `ProcessControl` capability before
PID 1 accepts them.
