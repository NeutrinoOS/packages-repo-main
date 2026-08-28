# OpenJDK 8 for Neutrino

This is an experimental headless-only OpenJDK 8 port.  It currently targets
Neutrino through OpenJDK's BSD/POSIX source path while platform work is in
progress.  `make configure` is the reproducible cross-configuration entry
point; `make hotspot` starts the first native build. It requires the Neutrino Newlib SDK and a host JDK 8 at
`/usr/lib/jvm/java-8-openjdk` (override with `BOOT_JDK=...`).

The intended first runtime milestone is enough of the JDK and IPv4 TCP stack to
run an older headless Notchian Minecraft server. It is not packaged or safe to
deploy yet. The build reaches HotSpot's native sources; further platform work
is required before it can produce a runnable JDK.
