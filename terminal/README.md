# Neutrino terminal

A basic graphical terminal client for the Neutrino desktop. It creates a VTY,
starts `@sys/binary/shell.elf` with that VTY as standard input/output/error,
renders the VTY cell buffer, and forwards desktop keyboard events.

Build the package with:

```sh
make package
```
