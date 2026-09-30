# OpenJDK 8 for Neutrino

This is an experimental headless-only OpenJDK 8 port.  It currently targets
Neutrino through OpenJDK's BSD/POSIX source path while platform work is in
progress. `make configure` is the reproducible cross-configuration entry
point; `make package` builds the headless runtime archive after configuration.
It requires the Neutrino Newlib SDK and a host JDK 8 at
`/usr/lib/jvm/java-8-openjdk` (override with `BOOT_JDK=...`).

The intended first runtime milestone is enough of the JDK and IPv4 TCP stack to
run an older headless Notchian Minecraft server. The launcher explicitly opts
into Neutrino's capability-gated writable/executable mapping policy before it
loads HotSpot. The invoking principal must hold the `MemoryWriteExecute`
capability; ordinary processes remain subject to W^X by default.

Packaging performs an ELF closure audit before producing the archive. Every
strong import must be provided by the launcher, the core JVM objects, or a
declared namespaced `DT_NEEDED` dependency, and every relocation must be one
the Neutrino loader supports. Unsupported platform operations such as process
creation and filesystem timestamp/permission mutation fail with an explicit
POSIX error instead of leaving unresolved loader symbols.

The packaged image keeps the native libraries in the JRE's standard
`lib/amd64` hierarchy for `System.loadLibrary`, and also installs the audited
objects in Neutrino's `openjdk-8` library namespace for loader dependency
resolution. `JDK_BUILD_DIR` selects the configured OpenJDK tree independently
from the package staging `BUILD_DIR`, so top-level isolated/release builds do
not accidentally look for a new OpenJDK configuration.
