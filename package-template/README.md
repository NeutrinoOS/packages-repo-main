# Neutrino Package Template

This is a small template for building a Neutrino userspace program.

## Build

```sh
make
```

The default output is:

```text
out/package-template.elf
```

Override the output program name with:

```sh
make PROGRAM=mytool
```

By default, every `src/*.cpp` file is compiled and linked into one program. To
choose files explicitly:

```sh
make PROGRAM=mytool SOURCES="src/main.cpp src/commands.cpp"
```

## Layout

- `src/` contains your program sources.
- `include/` contains the Neutrino userspace ABI headers. You likely shouldn't be touching this unless you're a kernel developer.
- `lib/` contains the tiny libc/neutrino support sources used by simple
  userspace programs. You likely shouldn't be touching this unless you're a kernel developer, either. This is a good place to add (bare-metal / Neutrino compatible) support libraries, though.
- `crt/` contains the entry point used before calling `main`. You probably don't need to touch this, either.

Programs should normally define:

```cpp
int main(uint64_t arg_ptr, uint64_t reserved);
```

`arg_ptr` points to the raw command-line argument string passed by Neutrino.

## Package

Neutrino packages are zip archives that are unpacked over the system root. For
example, a package named `musicplayer` should look like this:

```text
musicplayer.zip
| manifest.toml
| binary/
| | musicplayer.elf
| | whatever.elf
| config/
| | musicplayer/
| | | some_config.cfg
| scripts/
```

`scripts/` is reserved for future install script support. It should exist, but
it should normally be empty for now.

The package manifest should be formatted as such:

```toml
[test-package]
version = "0.0.1"
strategy = "over_root"
description = "Test package"
depends = []
```

`strategy` must stay as `"over_root"`; that is currently the only supported
install strategy. `depends` should list package names that must be installed for
this package to work.

This template includes a package staging area:

```text
package/
| manifest.toml
| config/
| | package-template/
| | | example.cfg
```

Edit `package/manifest.toml` before release. The manifest is used to generate your package's entry in the index. **Remember to bump your version!**

Build a package archive with:

```sh
make package
```

The default output is:

```text
out/package-template.zip
```

Override the binary and package name with:

```sh
make PROGRAM=musicplayer package
```

The package target deliberately uses uncompressed zip entries. Current `neupak`
can read zip archives, but it does not support compressed entries yet.

## Keeping The Template Current

The ABI headers are copied from the main Neutrino tree. Is something behind? Make an issue to notify someone, or refresh these files from Neutrino in a PR:

- `userspace/crt/syscall.hpp` -> `include/syscall.hpp`
- `shared/include/descriptors.hpp` -> `include/descriptors.hpp`
- `shared/include/neutrino_time.h` -> `include/neutrino_time.h`
- `userspace/libc/include/*.h` -> `include/`
- `userspace/libc/*.cpp` -> `lib/`
- `userspace/crt/crt0.s` -> `crt/crt0.s`
- etc... - really, anything inside crt/include/lib.
