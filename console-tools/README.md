# console-tools

Neutrino userspace package containing:

- `font`
- `sheet`

Build with:

```sh
make -C ../../neutrino/userspace newlib-sdk
make package
```

The neupak archive is written to `out/console-tools.zip`.

The package links against Neutrino's staged Newlib SDK. Ordinary C runtime
services come from Newlib; the package's descriptor helpers remain for
Neutrino-specific console, keyboard, and framebuffer operations.
