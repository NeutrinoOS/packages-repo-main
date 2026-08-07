# Writing Neutrino Programs

> **Migration note:** This guide describes the original freestanding runtime.
> Solitaire now uses Neutrino's Newlib SDK and a standard C++ `main` entry
> point. The descriptor sections remain relevant for Neutrino-specific
> devices.

This is a short guide to the userspace programming interface exposed by
Neutrino. It focuses on what a program can call once it is running: the small
libc, the syscall wrappers, files, descriptors, processes, memory, time, and
capability-related calls. Build and packaging details are intentionally kept
out of scope.

## Program Shape

Neutrino userspace programs are freestanding x86_64 ELF binaries. The CRT entry
point in `userspace/crt/crt0.s` calls:

```cpp
int main(uint64_t arg_ptr, uint64_t flags);
```

`arg_ptr` is either `0` or a pointer to a NUL-terminated argument string placed
in the child process by `exec()` or `child()`. Most existing command programs
cast it like this:

```cpp
int main(uint64_t arg_ptr, uint64_t) {
    const char* args = reinterpret_cast<const char*>(arg_ptr);
    // ...
}
```

The return value from `main` becomes the process exit status. Programs can also
call `exit(code)` from `userspace/crt/syscall.hpp`.

## Headers You Usually Want

Neutrino userspace code normally uses one of two layers:

- `<neutrino.h>`: small C-compatible convenience helpers for common tasks.
- `userspace/crt/syscall.hpp`: inline C++ syscall wrappers for the full current
  userspace ABI.

The minimal libc headers live in `userspace/libc/include`:

- `<string.h>` provides `memcpy`, `memset`, `memmove`, `memcmp`, `strcmp`,
  `strncmp`, `strlen`, and `strlcpy`.
- `<ctype.h>` currently provides `isspace`.
- `<neutrino.h>` provides Neutrino-specific helpers, such as `neutrino_parse_two_args`, `neutrino_copy_file`, and `neutrino_write_line`.

Shared kernel/userspace ABI definitions live under `shared/include`, especially
`descriptors.hpp` and `neutrino_time.h`.

## Convenience libc Helpers

`<neutrino.h>` is the friendly layer for small commands:

- `neutrino_parse_two_args(args, first, first_size, second, second_size)` parses
  exactly two whitespace-separated arguments.
- `neutrino_copy_file(source, dest)` copies a file and overwrites an existing
  regular destination.
- `neutrino_open_stdout()` returns the process stdout descriptor if available,
  or opens the console as a fallback.
- `neutrino_write(console, text)` writes a NUL-terminated string to a descriptor.
- `neutrino_write_line(console, text)` writes a string plus newline.
- `neutrino_get_time(out_time)` fills a `NeutrinoWallTime`.
- `neutrino_sync()` flushes filesystem/block cache state.
- `neutrino_shutdown()` asks the system to shut down.

For examples, see `userspace/programs/echo.cpp`, `cp.cpp`, `date.cpp`,
`sync.cpp`, and `shutdown.cpp`.

## Syscall Return Convention

The syscall wrappers generally return:

- `0` for success when no value is produced.
- A positive value or handle for successful calls that produce one.
- A negative value on failure.
- `0` bytes read for EOF on file and directory reads.

Many wrappers are very thin. Callers are expected to check negative returns and
close handles they successfully open.

`abi_major()` and `abi_minor()` report the userspace ABI version exposed by the
kernel. The current kernel table reports ABI `2.0`.

## Files and Directories

File handles are separate from descriptors. They are process-local integer
handles managed by the file syscalls:

- `file_open(path)` opens an existing file.
- `file_create(path)` creates a new file.
- `file_read(handle, buffer, length)` reads from the file's current position and
  advances it.
- `file_write(handle, buffer, length)` writes at the file's current position and
  advances it.
- `file_sync(handle)` flushes a file.
- `file_close(handle)` closes it.
- `file_remove(path)` removes a file.

Directory syscalls mirror this:

- `directory_open(path)` opens a directory.
- `directory_open_root()` opens the root directory.
- `directory_open_at(dir_handle, name)` opens a child directory.
- `directory_read(handle, &entry)` fills a `DirEntry`.
- `directory_close(handle)` closes it.
- `directory_create(path)` creates a directory.
- `directory_remove(path)` removes a directory.

`DirEntry` has `name`, `flags`, and `size`. Check
`DIR_ENTRY_FLAG_DIRECTORY` to distinguish directories from regular entries.

There are also relative file helpers:

- `file_open_at(dir_handle, name)`
- `file_create_at(dir_handle, name)`

Good examples are the main monorepo's `userspace/programs/cat.cpp`, `ls.cpp`, `mkdir.cpp`, `rmdir.cpp`, `rm.cpp`, `touch.cpp`, `write.cpp`, and `wc.cpp`.

## Descriptors

Descriptors are the general handle type for devices, streams, shared memory,
network endpoints, framebuffers, and other kernel objects. Descriptor ABI
constants are defined in `shared/include/descriptors.hpp`.

Open descriptors with:

```cpp
long handle = descriptor_open(type, resource_selector, requested_flags,
                              open_context);
```

Then use:

- `descriptor_read(handle, buffer, length, offset)`
- `descriptor_write(handle, buffer, length, offset)`
- `descriptor_close(handle)`
- `descriptor_get_type(handle)`
- `descriptor_get_flags(handle, extended)`
- `descriptor_test_flag(handle, flag)`
- `descriptor_get_property(handle, property, out, size)`
- `descriptor_set_property(handle, property, in, size)`
- `descriptor_wait(items, count)`

Standard descriptors are exposed at fixed handles when present:

- `kStandardInputDescriptor`
- `kStandardOutputDescriptor`
- `kStandardErrorDescriptor`

Use `process_get_standard_descriptor(0/1/2)` to validate and retrieve them.
Many programs fall back to opening a console descriptor when stdout is absent.

Common descriptor types include:

- `Console`, `Keyboard`, `Mouse`, `Framebuffer`, and `Vty`
- `BlockDevice`, `Disk`, and `Partition`
- `Pipe` and `SharedMemory`
- `CpuStats`, `TaskStats`, and `KernelLog`
- `NetDevice` and `NetEndpoint`
- `Pci` and `AudioOutput`

Descriptor flags describe behavior: readable, writable, seekable, mappable,
async, event source, device, block, and stream.

Properties are type-specific. For example, console scale/font/color, framebuffer
info/present, block geometry, disk and partition info, shared memory info, pipe
info, VTY cells/cursor/color, network device IPv4 config, audio status/control,
and so on.

## Console Output

For simple text output, prefer the helper layer:

```cpp
#include <neutrino.h>

int main(uint64_t arg_ptr, uint64_t) {
    const char* text = reinterpret_cast<const char*>(arg_ptr);
    long out = neutrino_open_stdout();
    neutrino_write_line(out, text != nullptr ? text : "");
    return 0;
}
```

If you need lower-level control, open a console descriptor directly:

```cpp
#include "descriptors.hpp"
#include "../crt/syscall.hpp"

long out = process_get_standard_descriptor(1);
if (out < 0) {
    out = descriptor_open(
        static_cast<uint32_t>(descriptor_defs::Type::Console), 0);
}
```

## Processes

Process creation and replacement are exposed through:

- `exec(path, args, flags, cwd)` replaces the current process image.
- `child(path, args, flags, cwd)` starts a child process and returns its pid.
- `child_with_stdio(path, args, flags, cwd, &stdio)` starts a child with
  explicit stdin/stdout/stderr descriptor handles.
- `yield()` yields the CPU.
- `sleep_ns`, `sleep_ms`, and `sleep_seconds` block for a duration.
- `setcwd(path)` changes the current working directory.
- `getcwd(buffer, length)` retrieves it.

The kernel copies up to 512 bytes of the argument string into the child process.
The copied string is passed to the child's `main` as `arg_ptr`.

## Time

`time_get(&wall_time)` fills a `NeutrinoWallTime`:

```cpp
struct NeutrinoWallTime {
    uint64_t unix_seconds;
    uint32_t nanoseconds;
    uint16_t year;
    uint8_t month, day, hour, minute, second, weekday;
};
```

The convenience wrapper is `neutrino_get_time(&wall_time)`.

## Memory Mapping

The memory syscalls are intentionally small:

- `map_anonymous(length, flags)` maps new anonymous user memory.
- `map_at(addr_hint, length, flags)` asks for a mapping near or at a hint.
- `unmap(addr, length)` releases it.

Use `MAP_WRITE` when writable memory is required. The wrappers return `nullptr`
on mapping failure.

Shared memory is descriptor-based:

- `shared_memory_open(name, length)`
- `shared_memory_get_info(handle, &info)`

`SharedMemoryInfo` reports the mapped base address and length.

## Devices and Special Helpers

`syscall.hpp` includes typed helpers for common descriptor families:

- Framebuffer: `framebuffer_open`, `framebuffer_open_slot`,
  `framebuffer_get_info`, and `framebuffer_present`.
- Mouse: `mouse_open`.
- Pipes: `pipe_open_new`, `pipe_open_existing`, and `pipe_get_info`.
- Network devices/endpoints: `net_device_open`, `net_device_get_info`,
  `net_device_get_ipv4_config`, `net_device_set_ipv4_config`,
  `net_endpoint_open_new`, `net_endpoint_open_existing`, and
  `net_endpoint_get_info`.
- Console fonts and scale: `console_get_scale`, `console_set_scale`,
  `console_get_font`, and `console_set_font`.
- Block devices: `mount_descriptor(block_handle, mount_name)` and
  `rescan_block_devices()`.

The kernel may require capabilities for some descriptor types or operations.
Hardware-oriented descriptors such as serial, block device, disk, partition, and
network device need hardware access unless the current principal is unconfined.
Monitoring descriptors such as CPU and task stats need monitor access.

## Users, Principals, and Capabilities

The security API is available from `syscall.hpp`:

- `user_create`, `user_find`, `user_bump_generation`, `user_set_password`, and
  `user_info`.
- `principal_create` and `principal_set`.
- `capability_grant(kind_value)` creates a capability token handle.
- `capability_pass(child_pid, handles, count)` copies capability handles into a
  child process.

Capability kinds are defined in `src/kernel/capabilities.hpp` and include
system settings write, block device read/write, process spawn, hardware access,
security management, stream, and monitor permissions.

Most ordinary command programs do not need to touch this API directly.

## A Tiny File-Reading Pattern

This is the usual shape for a program that opens a path and writes its content
to stdout:

```cpp
#include <stdint.h>
#include <string.h>
#include <neutrino.h>
#include "../crt/syscall.hpp"

int main(uint64_t arg_ptr, uint64_t) {
    const char* path = reinterpret_cast<const char*>(arg_ptr);
    if (path == nullptr || path[0] == '\0') {
        return 1;
    }

    long out = neutrino_open_stdout();
    long file = file_open(path);
    if (file < 0) {
        neutrino_write_line(out, "open failed");
        return 1;
    }

    char buffer[256];
    while (true) {
        long n = file_read(static_cast<uint32_t>(file), buffer, sizeof(buffer));
        if (n < 0) {
            file_close(static_cast<uint32_t>(file));
            return 1;
        }
        if (n == 0) {
            break;
        }
        descriptor_write(static_cast<uint32_t>(out), buffer,
                         static_cast<size_t>(n));
    }

    file_close(static_cast<uint32_t>(file));
    return 0;
}
```

## Where to Look Next

The best references are the existing programs in `userspace/programs`. Small,
readable examples include:

- `echo.cpp` for minimal output.
- `cat.cpp` for file reads and descriptor writes.
- `ls.cpp` for directory iteration.
- `sleep.cpp` and `date.cpp` for time.
- `shell.cpp` for process spawning and stdio handling.
- `top.cpp` and `dmesg.cpp` for monitoring/log descriptors.
- `netctl.cpp`, `tcpd.cpp`, and `dhcp.cpp` for network descriptors.
- `sdl2/src/src/audio/neutrino` and `sdl2/src/src/video/neutrino` for the
  SDL2 audio, desktop-window, and raw-framebuffer backends.
- `sheet.cpp` and `hexedit.cpp` for larger interactive programs.
