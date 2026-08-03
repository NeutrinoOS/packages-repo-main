# Programming Neutrino

> **Migration note:** This general programming guide documents the original
> freestanding package template and its raw process ABI. `console-tools` now
> uses Neutrino's Newlib SDK, standard `argc`/`argv` entry points, and libc for
> ordinary runtime services. Its descriptor APIs remain relevant for
> Neutrino-specific devices.

This manual is for writing ordinary Neutrino userspace programs from the
package template. It is less an ABI index than a field guide: what a process
looks like when it starts, how it talks to the kernel, how to parse its one
argument string, how to move bytes through files and descriptors, how to spawn
children, and what habits keep small system programs pleasant to debug.

The examples assume the package-template layout:

```text
crt/crt0.s
include/
lib/
src/main.cpp
Makefile
package/manifest.toml
```

Neutrino is still young, so the package template currently exposes a compact
userspace environment rather than a broad POSIX-style runtime. A program gets an
ELF image, a tiny CRT, a small libc, Neutrino-specific helper functions, and
direct syscall wrappers. Higher-level libraries will grow over time; for now,
more of the ordinary runtime work sits in the program itself. That is already
enough to write command line tools, installers, shells, file utilities, simple
graphics programs, network tools, and service-style programs, but it rewards a
more explicit style than mature Unix-like userspace.

If you already know C or C++, the main shift is not syntax. The shift is that
some conveniences you may be used to are not built out yet. You decide how
arguments become commands, how bytes become text, how much memory exists, which
handles are owned, and what "done" means for your program. Once you internalize
those current boundaries, writing a new Neutrino program feels straightforward:
choose the kernel objects you need, open them, move bytes through them, and
close them.

## How to Read This Manual

Read the first five sections in order if you are new to Neutrino programming:

1. The shape of a process.
2. The headers and freestanding C++ limits.
3. The build/package workflow.
4. Arguments, output, and errors.
5. Files, directories, descriptors, and processes.

After that, use the device-specific sections as reference material. You do not
need to understand framebuffers, network endpoints, and capabilities before
writing a useful file utility.

The quickest learning loop is:

1. Edit `src/main.cpp`.
2. Build with `make PROGRAM=mytool`.
3. Package with `make PROGRAM=mytool package` when you want to install it.
4. Run it under Neutrino.
5. Add one syscall family at a time.

## The Mental Model

A Neutrino program is a single executable image plus a small set of handles the
kernel knows about. There are two major handle families:

- File and directory handles: simple stream-like access to filesystem objects.
- Descriptors: general kernel objects such as consoles, framebuffers, pipes,
  devices, shared memory, stats, network endpoints, and audio output.

Most programs follow one of these shapes:

- Filter: read bytes from a file, descriptor, or argument and write transformed
  bytes somewhere else.
- Inspector: open a kernel object, query properties, and print a report.
- Mutator: create, remove, configure, mount, sync, or otherwise change system
  state.
- Launcher: spawn or replace processes.
- Interactive program: wait on input descriptors, update state, redraw output.
- Service: open resources once, then loop waiting for work.

When designing a new program, identify its shape first. That tells you which
parts of the ABI matter and which can be ignored.

### Picking the Right Interface

Use this table when you know what you want to do but not which calls are the
right entry point:

```text
Need                                    Start with
----                                    ----------
Print text                               neutrino_open_stdout, neutrino_write_line
Read or write a regular file             file_open/file_create, file_read/file_write
List a directory                         directory_open, directory_read
Work relative to an opened directory     file_open_at, directory_open_at
Talk to console/stdout/stderr            process_get_standard_descriptor, descriptor_write
Use keyboard or mouse input              descriptor_open, descriptor_read, descriptor_wait
Draw pixels                              framebuffer_open, framebuffer_get_info
Share memory with another process        shared_memory_open, shared_memory_get_info
Start another program                    child or child_with_stdio
Become another program                   exec
Sleep or yield                           sleep_ms/sleep_seconds/yield
Get wall-clock time                      time_get or neutrino_get_time
Read system stats/logs                   CpuStats/TaskStats/KernelLog descriptors
Configure hardware/network/storage       descriptor helpers plus capabilities
```

If the thing has a path, it is often a file or directory syscall. If the thing
is a device, stream, shared resource, or kernel service, it is usually a
descriptor.

## Building and Packaging

The package template builds one freestanding ELF program by default. The
important files are:

```text
src/main.cpp              your program
include/                  copied Neutrino ABI and tiny libc headers
lib/                      tiny libc and helper implementations
crt/crt0.s                process entry point
Makefile                  build and package rules
package/manifest.toml     package metadata
```

Build the default program with:

```sh
make
```

The output is:

```text
out/package-template.elf
```

Most real packages override the program name:

```sh
make PROGRAM=mytool
```

The output is then:

```text
out/mytool.elf
```

Every `src/*.cpp` file is compiled and linked into the program unless you
override `SOURCES`:

```sh
make PROGRAM=mytool SOURCES="src/main.cpp src/parse.cpp src/render.cpp"
```

The build uses freestanding flags, disables the red zone and SIMD families, and
links without the host standard library. If code builds on your host with
ordinary `g++` but fails here, check for accidental dependencies on hosted
runtime features such as allocation, exceptions, iostreams, libc calls not
provided by the template, or compiler-generated runtime helpers.

Build a package archive with:

```sh
make PROGRAM=mytool package
```

The package output is:

```text
out/mytool.zip
```

Neutrino packages are zip archives unpacked over the system root. The generated
archive contains:

```text
manifest.toml
binary/mytool.elf
config/
scripts/
```

`scripts/` exists for future install script support and is normally empty. The
zip entries are uncompressed because current `neupak` support expects
uncompressed entries.

Before releasing a real package, edit `package/manifest.toml`:

```toml
[mytool]
version = "0.1.0"
strategy = "over_root"
description = "Short description of the tool"
depends = []
```

`strategy` must currently stay `"over_root"`. `depends` lists package names
that must already be installed for your package to work.

This manual lives in the template, so its examples do not modify the example
manifest version. For your own standalone package, bump the manifest version
according to semantic versioning when behavior changes.

## Organizing a Program

For a tiny command, one `src/main.cpp` is perfect. For anything larger, split
by responsibility rather than by syscall family:

```text
src/main.cpp       entry point, usage, top-level command dispatch
src/args.cpp       argument scanner or command parser
src/io.cpp         write_all, read loops, formatting helpers
src/model.cpp      program-specific data structures
src/render.cpp     framebuffer, VTY, or text rendering
```

Keep `main()` boring:

1. Normalize `arg_ptr`.
2. Open stdout or the descriptors needed for diagnostics.
3. Parse arguments.
4. Open resources.
5. Call the program's real work function.
6. Close resources and return an exit code.

That shape makes failures easier to explain and makes it obvious which handles
exist at each point in the program.

## The Shape of a Program

Neutrino userspace programs are freestanding x86_64 ELF binaries. The assembly
entry point in `crt/crt0.s` enters your program by calling:

```cpp
int main(uint64_t arg_ptr, uint64_t flags);
```

`arg_ptr` is either `0` or the address of a NUL-terminated argument string
placed in the new process by `exec()`, `child()`, or `child_with_stdio()`.
`flags` is reserved for process-start metadata. Most programs ignore it.

The return value from `main` becomes the process exit status. You can also
terminate immediately with `exit(code)` from `include/syscall.hpp`.

A useful skeleton is:

```cpp
#include <stdint.h>
#include <neutrino.h>

int main(uint64_t arg_ptr, uint64_t) {
    const char* args = reinterpret_cast<const char*>(arg_ptr);
    long out = neutrino_open_stdout();

    if (args == nullptr || args[0] == '\0') {
        neutrino_write_line(out, "usage: example <text>");
        return 1;
    }

    neutrino_write_line(out, args);
    return 0;
}
```

### What the Early Runtime Does Not Provide Yet

Do not assume POSIX process state exists yet. In particular, the current
template does not provide:

- An `argc`/`argv` array.
- `stdin`/`stdout`/`stderr` `FILE*` objects.
- An environment variable table.
- A current locale.
- A heap unless you build one yourself on top of mapped memory.
- Signals.
- Kernel-side path expansion, shell quoting, or wildcard expansion.

These are current boundaries, not design commandments. Code written against the
template today should handle them directly, and future libraries can smooth over
some of them without changing the basic syscall model.

The shell or parent process passes a single string. Until a richer process
runtime exists, your program decides how much structure to impose on it.

### Exit Status

Use `0` for success. Use a nonzero value for failure. Existing commands usually
return `1` for ordinary usage or runtime failures. Keep exit codes simple unless
your program has a documented reason to distinguish cases.

When a program owns resources, close or flush them before returning. Returning
from `main` is fine, but explicit cleanup makes programs easier to modify later.

## The Headers

The template has three practical layers.

The small libc headers are in `include/`:

- `<stdint.h>` and `<stddef.h>` for fixed-width integers and sizes.
- `<string.h>` for `memcpy`, `memset`, `memmove`, `memcmp`, `strcmp`,
  `strncmp`, `strlen`, and `strlcpy`.
- `<ctype.h>` for `isspace`.

The convenience layer is:

- `<neutrino.h>` for small C-compatible helpers used by command programs.

The ABI layer is:

- `include/syscall.hpp` for inline syscall wrappers.
- `include/descriptors.hpp` for descriptor types, flags, properties, and
  descriptor-specific structs.
- `include/neutrino_time.h` for wall-clock time structures.

For small utilities, start with `<neutrino.h>`. Drop to `syscall.hpp` when you
need a syscall not wrapped by the helper layer.

## A Freestanding C++ Dialect

The template is C++-friendly, but it is not a hosted C++ runtime yet.

Good fits:

- Plain structs and enums.
- Stack buffers.
- Fixed-size arrays.
- Simple RAII wrappers that only call syscalls in destructors.
- `static` helper functions in the same translation unit.
- Explicit error checks.

Features that need more runtime support than the template currently ships:

- Exceptions.
- RTTI-heavy designs.
- Standard library containers and streams.
- Global constructors that assume a full C++ runtime.
- Allocation-heavy code.

The practical style today is close to classic systems programming: fixed
buffers, explicit ownership, and clear failure paths.

## Arguments

The kernel copies up to 512 bytes of argument string into the child process.
The copied string is passed as `arg_ptr`. If no argument string was supplied,
`arg_ptr` may be `0`.

The current helper layer does not include a universal quoting parser. Many
command programs use whitespace splitting. If your program needs quoted
strings, paths with spaces, subcommands, or flags, implement that behavior
locally and document it in your usage text.

### Defensive Argument Handling

Always normalize the raw pointer first:

```cpp
static const char* args_from_ptr(uint64_t arg_ptr) {
    const char* args = reinterpret_cast<const char*>(arg_ptr);
    return args != nullptr ? args : "";
}
```

Check for empty input before parsing:

```cpp
const char* args = args_from_ptr(arg_ptr);
if (args[0] == '\0') {
    neutrino_write_line(out, "usage: tool <path>");
    return 1;
}
```

### Parsing Two Arguments

For the common `cp source dest` style, use:

```cpp
char first[128];
char second[128];
if (!neutrino_parse_two_args(args, first, sizeof(first), second, sizeof(second))) {
    neutrino_write_line(out, "usage: cp <source> <dest>");
    return 1;
}
```

This parser expects exactly two whitespace-separated arguments. It is good for
simple command tools. It is not a shell language.

### A Small Token Scanner

When you need more than two arguments, a tiny scanner is often clearer than
trying to be clever:

```cpp
#include <ctype.h>

static const char* skip_space(const char* p) {
    while (*p != '\0' && isspace(static_cast<unsigned char>(*p))) {
        ++p;
    }
    return p;
}

static const char* read_token(const char* p, char* out, size_t out_size) {
    p = skip_space(p);
    if (*p == '\0' || out_size == 0) {
        return nullptr;
    }

    size_t used = 0;
    while (*p != '\0' && !isspace(static_cast<unsigned char>(*p))) {
        if (used + 1 < out_size) {
            out[used++] = *p;
        }
        ++p;
    }
    out[used] = '\0';
    return p;
}
```

Prefer truncation-resistant parsing: keep room for a trailing NUL and treat
oversized input as an error if truncation would be surprising.

## Output

Most text-mode programs should write to stdout if the parent provided it, and
fall back to the console otherwise:

```cpp
long out = neutrino_open_stdout();
neutrino_write_line(out, "hello");
```

`neutrino_write(out, text)` writes a NUL-terminated string.
`neutrino_write_line(out, text)` writes the string and a newline.

These helpers are intentionally small. They do not format integers, allocate
temporary strings, or retry forever. If you need formatting, build it explicitly
with fixed buffers.

### Writing Raw Bytes

For binary data or data already in a buffer, use `descriptor_write()`:

```cpp
const char bytes[] = { 'O', 'K', '\n' };
long written = descriptor_write(static_cast<uint32_t>(out), bytes, sizeof(bytes));
if (written < 0) {
    return 1;
}
```

Descriptor writes return a negative value on error. Depending on descriptor
type, a successful write may write fewer bytes than requested. For regular
console-style output this is uncommon, but robust stream code should loop until
all bytes are written or an error occurs.

### Minimal Integer Formatting

A tiny unsigned decimal formatter is useful in many programs:

```cpp
static void u64_to_decimal(uint64_t value, char* out, size_t out_size) {
    if (out_size == 0) {
        return;
    }

    char tmp[21];
    size_t n = 0;
    do {
        tmp[n++] = static_cast<char>('0' + (value % 10));
        value /= 10;
    } while (value != 0 && n < sizeof(tmp));

    size_t used = 0;
    while (n > 0 && used + 1 < out_size) {
        out[used++] = tmp[--n];
    }
    out[used] = '\0';
}
```

Keep formatting helpers local unless several programs truly need the same one.

### Combining Text Safely

Until a formatting library is available, assemble short messages explicitly:

```cpp
static bool append(char* out, size_t out_size, const char* text) {
    size_t have = strlen(out);
    size_t add = strlen(text);
    if (have + add + 1 > out_size) {
        return false;
    }
    memcpy(out + have, text, add + 1);
    return true;
}
```

Initialize the destination before appending:

```cpp
char message[160];
message[0] = '\0';
append(message, sizeof(message), "open failed: ");
append(message, sizeof(message), path);
neutrino_write_line(out, message);
```

This style is a temporary substitute for a formatting library, but it is
predictable and works in the current freestanding environment.

### Printing Numbers With Labels

For report-style tools, combine the decimal helper with small writes:

```cpp
static void write_u64_line(long out, const char* label, uint64_t value) {
    char digits[21];
    u64_to_decimal(value, digits, sizeof(digits));
    neutrino_write(out, label);
    neutrino_write_line(out, digits);
}
```

Then code that queries kernel objects can stay readable:

```cpp
write_u64_line(out, "bytes: ", entry.size);
```

## Errors

The syscall wrappers generally return:

- `0` for success when no value is produced.
- A positive value or handle for successful calls that produce one.
- A negative value on failure.
- `0` bytes read for EOF on file and directory reads.

The template does not currently provide an `errno` interface. Treat negative
return values as failure and print a useful message in the context of the
operation:

```cpp
long file = file_open(path);
if (file < 0) {
    neutrino_write(out, "open failed: ");
    neutrino_write_line(out, path);
    return 1;
}
```

Do not expose raw negative numbers as your only diagnostic unless you are
writing a debugging tool. Say what operation failed and which object it was
trying to use.

## Files

File handles are process-local integer handles managed by the file syscalls.
They are separate from descriptors.

Open or create files with:

```cpp
long file = file_open(path);
long created = file_create(path);
```

Use:

- `file_read(handle, buffer, length)`
- `file_write(handle, buffer, length)`
- `file_sync(handle)`
- `file_close(handle)`
- `file_remove(path)`

Reads and writes use the file's current position and advance it. The current
template ABI does not expose a seek syscall, so simple file programs are
naturally stream-shaped.

### Reading a Whole File as a Stream

```cpp
static int copy_file_to_descriptor(const char* path, uint32_t out) {
    long file = file_open(path);
    if (file < 0) {
        return -1;
    }

    char buffer[512];
    while (true) {
        long n = file_read(static_cast<uint32_t>(file), buffer, sizeof(buffer));
        if (n < 0) {
            file_close(static_cast<uint32_t>(file));
            return -1;
        }
        if (n == 0) {
            break;
        }

        size_t offset = 0;
        while (offset < static_cast<size_t>(n)) {
            long wrote = descriptor_write(out,
                                          buffer + offset,
                                          static_cast<size_t>(n) - offset);
            if (wrote < 0) {
                file_close(static_cast<uint32_t>(file));
                return -1;
            }
            offset += static_cast<size_t>(wrote);
        }
    }

    file_close(static_cast<uint32_t>(file));
    return 0;
}
```

This is the core pattern behind `cat`, file viewers, hashers, counters, and
copy tools: open, loop, check negative, stop on zero, close.

### Creating and Writing Files

```cpp
long file = file_create(path);
if (file < 0) {
    return 1;
}

const char text[] = "created by my program\n";
long wrote = file_write(static_cast<uint32_t>(file), text, sizeof(text) - 1);
if (wrote < 0) {
    file_close(static_cast<uint32_t>(file));
    return 1;
}

file_sync(static_cast<uint32_t>(file));
file_close(static_cast<uint32_t>(file));
```

Call `file_sync()` when the program's job is not complete until data reaches
the filesystem layer. For short-lived tools that create important files, syncing
before close is a good habit.

### Copying Files

For ordinary whole-file copies, the helper layer provides:

```cpp
if (!neutrino_copy_file(source, dest)) {
    neutrino_write_line(out, "copy failed");
    return 1;
}
```

Use the helper when you just need the conventional behavior. Write the loop
yourself when you need progress output, filtering, transformation, or special
error handling.

## Directories

Directory handles are also process-local handles.

Use:

- `directory_open(path)`
- `directory_open_root()`
- `directory_open_at(dir_handle, name)`
- `directory_read(handle, &entry)`
- `directory_close(handle)`
- `directory_create(path)`
- `directory_remove(path)`

Directory entries use:

```cpp
struct DirEntry {
    char name[64];
    uint32_t flags;
    uint32_t reserved;
    uint64_t size;
};
```

Check `DIR_ENTRY_FLAG_DIRECTORY` to distinguish directories from regular
entries.

### Directory Iteration

```cpp
long dir = directory_open(path);
if (dir < 0) {
    return 1;
}

DirEntry entry;
while (true) {
    long n = directory_read(static_cast<uint32_t>(dir), &entry);
    if (n < 0) {
        directory_close(static_cast<uint32_t>(dir));
        return 1;
    }
    if (n == 0) {
        break;
    }

    neutrino_write_line(out, entry.name);
}

directory_close(static_cast<uint32_t>(dir));
```

Treat `0` from `directory_read()` as end of directory, not as an error.

### Relative Opens

When walking a directory tree, prefer relative helpers over constructing larger
path strings when possible:

- `directory_open_at(dir_handle, name)`
- `file_open_at(dir_handle, name)`
- `file_create_at(dir_handle, name)`

This avoids fixed-buffer path joins and makes traversal code less fragile.

## Paths

Path interpretation belongs to the filesystem layer and the current working
directory. Programs can read and change their working directory with:

- `getcwd(buffer, length)`
- `setcwd(path)`

For tools that accept paths, print the path back in error messages. It is much
easier to debug `remove failed: /system/bin/foo` than `remove failed`.

When joining paths manually, remember:

- Keep room for the slash and trailing NUL.
- Handle root specially if you care about avoiding `//`.
- Reject names that do not fit instead of silently producing a different path.

## Descriptors

Descriptors are the general kernel object handle type. They represent devices,
streams, shared memory, network endpoints, framebuffers, virtual terminals, and
monitoring interfaces. Descriptor constants and structures live in
`include/descriptors.hpp`.

Open descriptors with:

```cpp
long handle = descriptor_open(type, resource_selector, requested_flags, open_context);
```

The shorter overloads in `syscall.hpp` let you omit trailing arguments when you
do not need them.

Use:

- `descriptor_read(handle, buffer, length, offset)`
- `descriptor_write(handle, buffer, length, offset)`
- `descriptor_close(handle)`
- `descriptor_get_type(handle)`
- `descriptor_get_flags(handle, extended)`
- `descriptor_test_flag(handle, flag)`
- `descriptor_get_property(handle, property, out, size)`
- `descriptor_set_property(handle, property, in, size)`
- `descriptor_wait(items, count)`

### Files Are Not Descriptors

File handles and descriptor handles are different namespaces. Use file syscalls
with handles returned by `file_open()` or `file_create()`. Use descriptor
syscalls with handles returned by `descriptor_open()` and descriptor-specific
helpers.

This distinction is easy to forget because both are integers. Name variables
accordingly:

```cpp
long file = file_open(path);
long console = neutrino_open_stdout();
```

### Standard Descriptors

When present, standard descriptors use fixed handles:

- `kStandardInputDescriptor`
- `kStandardOutputDescriptor`
- `kStandardErrorDescriptor`

Use `process_get_standard_descriptor(0)`, `process_get_standard_descriptor(1)`,
or `process_get_standard_descriptor(2)` to validate and retrieve them.

The helper `neutrino_open_stdout()` does the usual stdout-or-console fallback.
For stderr, ask for descriptor `2` and fall back to stdout or console if your
program needs a best-effort diagnostic path.

### Descriptor Types

Common descriptor types include:

- `Console`, `Keyboard`, `Mouse`, `Framebuffer`, and `Vty`
- `BlockDevice`, `Disk`, and `Partition`
- `Pipe` and `SharedMemory`
- `CpuStats`, `TaskStats`, and `KernelLog`
- `NetDevice` and `NetEndpoint`
- `Pci` and `AudioOutput`

Descriptor flags describe behavior such as readable, writable, seekable,
mappable, async, event source, device, block, and stream.

Properties are type-specific. Examples include console scale/font/color,
framebuffer info/present, block geometry, disk and partition info, shared memory
info, pipe info, VTY cells/cursor/color, network device IPv4 config, and audio
status/control.

### Descriptor Properties

Properties are small binary structs. Always pass the exact struct size expected
by the property:

```cpp
descriptor_defs::FramebufferInfo info;
long rc = descriptor_get_property(fb,
                                  static_cast<uint32_t>(descriptor_defs::Property::FramebufferInfo),
                                  &info,
                                  sizeof(info));
if (rc < 0) {
    return 1;
}
```

If you change a property struct in a local experiment, rebuild everything that
shares the header. Kernel and userspace must agree on layout.

## Waiting and Events

`descriptor_wait(items, count)` waits on descriptor event sources. It is the
primitive for interactive programs that need to sleep until input arrives rather
than poll in a hot loop.

The exact event bits are descriptor-family-specific, so inspect
`descriptors.hpp` and existing interactive programs before designing a new
event loop. The usual structure is:

```cpp
descriptor_defs::DescriptorWait waits[2];
memset(waits, 0, sizeof(waits));
waits[0].handle = keyboard;
waits[1].handle = mouse;

long rc = descriptor_wait(waits, 2);
if (rc < 0) {
    return 1;
}
```

Then check the returned wait records according to the descriptor type.

## Processes

Neutrino exposes process replacement and child creation through:

- `exec(path, args, flags, cwd)`
- `child(path, args, flags, cwd)`
- `child_with_stdio(path, args, flags, cwd, &stdio)`
- `yield()`
- `sleep_ns(duration_ns)`
- `sleep_ms(duration_ms)`
- `sleep_seconds(duration_seconds)`
- `setcwd(path)`
- `getcwd(buffer, length)`

`exec()` replaces the current process image. It does not return on success.
Use it for launchers that should become the target program.

`child()` starts another program and returns its pid on success. Use it for
shells, supervisors, service launchers, and tools that need to keep running
after starting another process.

### Spawning a Child

```cpp
long pid = child("/system/bin/echo", "hello from child", 0, nullptr);
if (pid < 0) {
    neutrino_write_line(out, "child failed");
    return 1;
}
```

The parent does not automatically receive the child's output unless stdio is
configured appropriately by the shell or by `child_with_stdio()`.

### Explicit Stdio

Use `child_with_stdio()` when writing shells, pipelines, tests, or services
that need to wire a child to known descriptors:

```cpp
ProcessStdioConfig stdio;
stdio.stdin_handle = kInvalidDescriptor;
stdio.stdout_handle = static_cast<uint32_t>(out);
stdio.stderr_handle = static_cast<uint32_t>(out);
stdio.reserved = 0;

long pid = child_with_stdio(path, args, 0, nullptr, &stdio);
if (pid < 0) {
    return 1;
}
```

Only pass handles that are valid in the parent and meaningful for the child.

### Current Working Directory

Pass a `cwd` argument to `exec()` or `child()` when the child should start in a
specific directory. Use `nullptr` to inherit or use the default behavior
provided by the kernel/syscall wrapper.

For a command that changes its own directory:

```cpp
if (setcwd(path) < 0) {
    neutrino_write_line(out, "cd failed");
    return 1;
}
```

## Time and Sleeping

`time_get(&wall_time)` fills:

```cpp
struct NeutrinoWallTime {
    uint64_t unix_seconds;
    uint32_t nanoseconds;
    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
    uint8_t weekday;
};
```

The convenience wrapper is `neutrino_get_time(&wall_time)`.

Use `sleep_ms()` or `sleep_seconds()` for ordinary delays. Use `sleep_ns()` when
you need the unit, but remember that timer precision and scheduling latency are
kernel realities, not promises of exact wakeup.

## Memory

The memory syscalls are small:

- `map_anonymous(length, flags)`
- `map_at(addr_hint, length, flags)`
- `unmap(addr, length)`

Use `MAP_WRITE` for writable memory. Mapping wrappers return `nullptr` on
failure.

```cpp
void* memory = map_anonymous(4096, MAP_WRITE);
if (memory == nullptr) {
    return 1;
}

memset(memory, 0, 4096);
unmap(memory, 4096);
```

The template does not currently include a general-purpose allocator. For many
utilities, stack buffers are simpler and safer. For larger tools, you can build
a tiny arena on top of one anonymous mapping:

```cpp
struct Arena {
    uint8_t* base;
    size_t used;
    size_t size;
};

static void* arena_alloc(Arena* arena, size_t size) {
    size = (size + 7u) & ~static_cast<size_t>(7u);
    if (arena->used + size > arena->size) {
        return nullptr;
    }
    void* ptr = arena->base + arena->used;
    arena->used += size;
    return ptr;
}
```

Keep ownership simple. Programs are easier to audit when every mapping has a
clear lifetime.

## Shared Memory

Shared memory is descriptor-based:

- `shared_memory_open(name, length)`
- `shared_memory_get_info(handle, &info)`

`SharedMemoryInfo` reports the mapped base address and length. Use shared
memory when two processes need a common buffer and a descriptor or protocol
gives them a way to coordinate access.

Shared memory gives you bytes, not synchronization. If two processes write the
same region, design a protocol for ownership, readiness, and versioning.

## Framebuffer Programs

Framebuffer access is descriptor-based. The helper wrappers include:

- `framebuffer_open()`
- `framebuffer_open_slot(slot)`
- `framebuffer_get_info(handle, &info)`
- `framebuffer_present(handle)`

The usual pattern is:

1. Open the framebuffer.
2. Query its info.
3. Write pixels into the mapped buffer described by the info struct.
4. Present when ready.

Always respect the reported pitch, dimensions, format fields, and buffer size.
Do not assume a tightly packed `width * height * 4` layout unless the returned
info says that is what you have.

For animation, combine a framebuffer loop with `sleep_ms()` or descriptor
events. Avoid busy loops that repaint as fast as the CPU allows.

## Keyboard, Mouse, and VTY Programs

Keyboard, mouse, console, and VTY interfaces are descriptors. For interactive
programs:

- Open the input descriptors you need.
- Query any geometry or mode properties.
- Use `descriptor_wait()` when possible.
- Read events into the structs defined by `descriptors.hpp`.
- Redraw only what changed when the interface is slow.

Interactive code benefits from a small state struct:

```cpp
struct AppState {
    int cursor_x;
    int cursor_y;
    bool running;
};
```

Keep event decoding separate from drawing. That makes it much easier to add
keyboard shortcuts or mouse behavior later.

## Pipes and Streams

Pipes are descriptors. The wrappers include:

- `pipe_open_new()`
- `pipe_open_existing(id)`
- `pipe_get_info(handle, &info)`

A stream may report `kDescriptorWouldBlock` when it has no data available in a
nonblocking or async situation. Treat that value differently from fatal errors
when writing event-driven programs.

For stream copying, use the same loop shape as file copying: read into a fixed
buffer, write until consumed, stop on EOF or a protocol-specific close event.

## Network Programs

Network devices and endpoints are descriptor families. The wrappers include:

- `net_device_open()`
- `net_device_get_info(handle, &info)`
- `net_device_get_ipv4_config(handle, &config)`
- `net_device_set_ipv4_config(handle, &config)`
- `net_endpoint_open_new(...)`
- `net_endpoint_open_existing(...)`
- `net_endpoint_get_info(handle, &info)`

Network tools often require capabilities. Ordinary applications should expect
open or configuration calls to fail when the current principal is not allowed to
manage network state.

Keep network protocols explicit and bounded. Avoid assuming that a read returns
a whole application message unless the endpoint contract says so.

## Audio Output

Audio output is descriptor-based. Query status/control properties before
streaming and handle short writes. Audio programs are timing-sensitive, so keep
buffer sizes deliberate and avoid printing from the hot path unless debugging.

## Block Devices and Mounts

Block, disk, and partition descriptors expose storage. The helper wrappers
include:

- `mount_descriptor(block_handle, mount_name)`
- `rescan_block_devices()`

These operations generally need stronger capabilities than ordinary file
programs. Tools that manipulate storage should be conservative: validate
arguments, print target devices clearly, and sync after changes.

## Users, Principals, and Capabilities

The security API is available from `syscall.hpp`:

- `user_create(...)`
- `user_find(...)`
- `user_bump_generation(...)`
- `user_set_password(...)`
- `user_info(...)`
- `principal_create(...)`
- `principal_set(...)`
- `capability_grant(kind_value)`
- `capability_pass(child_pid, handles, count)`

Most programs do not need this API. Use it for login tools, service launchers,
administrative utilities, and tests of security behavior.

Capabilities are handles. If a parent wants a child to perform privileged work,
the parent can grant or pass capability handles to that child. Design such tools
so the privileged part is as small and explicit as possible.

Capability kinds include system settings write, block device read/write,
process spawn, hardware access, security management, stream, and monitor
permissions. Check the kernel capability definitions when writing a tool that
needs one.

## ABI Versioning

`abi_major()` and `abi_minor()` report the userspace ABI version exposed by the
kernel. The current kernel table reports ABI `1.0`.

Use the ABI version when a program can run against multiple kernel/template
versions and needs to choose behavior:

```cpp
if (abi_major() != 1) {
    neutrino_write_line(out, "unsupported ABI");
    return 1;
}
```

For ordinary package-template programs, compiling against the template headers
and running on a matching Neutrino system is the expected path.

## Resource Ownership

Every successful open should have an obvious close. In small functions, direct
cleanup is usually clearer than abstraction:

```cpp
long file = file_open(path);
if (file < 0) {
    return 1;
}

long rc = do_work(static_cast<uint32_t>(file));
file_close(static_cast<uint32_t>(file));
return rc < 0 ? 1 : 0;
```

For larger C++ programs, a tiny handle wrapper is worthwhile:

```cpp
struct FileHandle {
    long value;

    explicit FileHandle(long handle) : value(handle) {}
    ~FileHandle() {
        if (value >= 0) {
            file_close(static_cast<uint32_t>(value));
        }
    }

    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;
};
```

Use RAII only when the project is already C++ enough to make it natural. For
tiny command files, straight-line cleanup is fine.

## Fixed Buffers

Fixed buffers are normal in Neutrino programs today. Use them carefully:

- Pick sizes based on the ABI or command contract.
- Keep one byte for the trailing NUL in strings.
- Reject input that does not fit.
- Avoid recursive path building with large stack buffers.
- Prefer streaming over reading entire files into memory.

`DirEntry::name` is 64 bytes. The process argument string is copied up to 512
bytes. Those sizes should shape your interfaces.

## Usage Text

Good usage text is short and exact:

```text
usage: copy <source> <dest>
```

Print usage when arguments are missing or malformed. Print operation-specific
errors when syscalls fail:

```text
open failed: /missing/file
```

Avoid long help screens in small system tools unless the tool genuinely has
subcommands or options.

## Designing Something New

When you sit down to write a new Neutrino program, work from the outside in.
Answer these questions before writing syscalls:

1. What does the program receive?
2. What does it produce?
3. What kernel objects does it need?
4. Does it run once, loop for events, or launch something else?
5. What resources must be closed or synced before success?
6. What should the user see when each operation fails?

For example, suppose you want to write `wherebig`, a utility that lists files
in a directory larger than a threshold.

The shape is:

```text
input:      "<directory> <minimum-bytes>"
output:     matching names and sizes on stdout
kernel:     directory_open, directory_read, directory_close
lifetime:   run once
cleanup:    close the directory
failure:    usage, bad number, open failure, read failure
```

That design already tells you most of the code. You need an argument parser, a
decimal parser, directory iteration, and output formatting. You do not need
memory mapping, process spawning, capabilities, or descriptors beyond stdout.

### Parsing a Decimal Number

```cpp
static bool parse_u64(const char* text, uint64_t* out) {
    if (text == nullptr || text[0] == '\0' || out == nullptr) {
        return false;
    }

    uint64_t value = 0;
    for (const char* p = text; *p != '\0'; ++p) {
        if (*p < '0' || *p > '9') {
            return false;
        }
        uint64_t digit = static_cast<uint64_t>(*p - '0');
        uint64_t next = value * 10 + digit;
        if (next < value) {
            return false;
        }
        value = next;
    }

    *out = value;
    return true;
}
```

### A Complete Example: `wherebig`

This is not a standard utility. It is a small example of making something new
out of the primitives in this manual.

```cpp
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <neutrino.h>
#include "syscall.hpp"

static void u64_to_decimal(uint64_t value, char* out, size_t out_size) {
    if (out_size == 0) {
        return;
    }

    char tmp[21];
    size_t n = 0;
    do {
        tmp[n++] = static_cast<char>('0' + (value % 10));
        value /= 10;
    } while (value != 0 && n < sizeof(tmp));

    size_t used = 0;
    while (n > 0 && used + 1 < out_size) {
        out[used++] = tmp[--n];
    }
    out[used] = '\0';
}

static bool parse_u64(const char* text, uint64_t* out) {
    if (text == nullptr || text[0] == '\0' || out == nullptr) {
        return false;
    }

    uint64_t value = 0;
    for (const char* p = text; *p != '\0'; ++p) {
        if (*p < '0' || *p > '9') {
            return false;
        }
        uint64_t digit = static_cast<uint64_t>(*p - '0');
        uint64_t next = value * 10 + digit;
        if (next < value) {
            return false;
        }
        value = next;
    }

    *out = value;
    return true;
}

int main(uint64_t arg_ptr, uint64_t) {
    const char* args = reinterpret_cast<const char*>(arg_ptr);
    long out = neutrino_open_stdout();

    char path[128];
    char min_text[32];
    if (!neutrino_parse_two_args(args, path, sizeof(path), min_text, sizeof(min_text))) {
        neutrino_write_line(out, "usage: wherebig <directory> <minimum-bytes>");
        return 1;
    }

    uint64_t minimum = 0;
    if (!parse_u64(min_text, &minimum)) {
        neutrino_write_line(out, "bad minimum byte count");
        return 1;
    }

    long dir = directory_open(path);
    if (dir < 0) {
        neutrino_write(out, "open directory failed: ");
        neutrino_write_line(out, path);
        return 1;
    }

    DirEntry entry;
    while (true) {
        long n = directory_read(static_cast<uint32_t>(dir), &entry);
        if (n < 0) {
            neutrino_write_line(out, "directory read failed");
            directory_close(static_cast<uint32_t>(dir));
            return 1;
        }
        if (n == 0) {
            break;
        }
        if ((entry.flags & DIR_ENTRY_FLAG_DIRECTORY) != 0) {
            continue;
        }
        if (entry.size < minimum) {
            continue;
        }

        char size_text[21];
        u64_to_decimal(entry.size, size_text, sizeof(size_text));
        neutrino_write(out, size_text);
        neutrino_write(out, " ");
        neutrino_write_line(out, entry.name);
    }

    directory_close(static_cast<uint32_t>(dir));
    return 0;
}
```

The important thing is the method. The program did not require a new runtime or
a framework. It required naming the program shape, choosing two syscall
families, writing two tiny support functions, and checking every boundary.

## A Complete Example: `cat`

```cpp
#include <stdint.h>
#include <stddef.h>
#include <neutrino.h>
#include "syscall.hpp"

static int write_all(uint32_t out, const char* data, size_t size) {
    size_t offset = 0;
    while (offset < size) {
        long n = descriptor_write(out, data + offset, size - offset);
        if (n < 0) {
            return -1;
        }
        if (n == 0) {
            return -1;
        }
        offset += static_cast<size_t>(n);
    }
    return 0;
}

int main(uint64_t arg_ptr, uint64_t) {
    const char* path = reinterpret_cast<const char*>(arg_ptr);
    long out = neutrino_open_stdout();

    if (path == nullptr || path[0] == '\0') {
        neutrino_write_line(out, "usage: cat <path>");
        return 1;
    }

    long file = file_open(path);
    if (file < 0) {
        neutrino_write(out, "open failed: ");
        neutrino_write_line(out, path);
        return 1;
    }

    char buffer[512];
    while (true) {
        long n = file_read(static_cast<uint32_t>(file), buffer, sizeof(buffer));
        if (n < 0) {
            neutrino_write_line(out, "read failed");
            file_close(static_cast<uint32_t>(file));
            return 1;
        }
        if (n == 0) {
            break;
        }
        if (write_all(static_cast<uint32_t>(out), buffer, static_cast<size_t>(n)) < 0) {
            file_close(static_cast<uint32_t>(file));
            return 1;
        }
    }

    file_close(static_cast<uint32_t>(file));
    return 0;
}
```

The important details are not the specific buffer size. They are the loop
shape, the negative checks, the EOF check, and closing the file on every path.

## A Complete Example: `ls`

```cpp
#include <stdint.h>
#include <neutrino.h>
#include "syscall.hpp"

int main(uint64_t arg_ptr, uint64_t) {
    const char* path = reinterpret_cast<const char*>(arg_ptr);
    if (path == nullptr || path[0] == '\0') {
        path = ".";
    }

    long out = neutrino_open_stdout();
    long dir = directory_open(path);
    if (dir < 0) {
        neutrino_write(out, "open directory failed: ");
        neutrino_write_line(out, path);
        return 1;
    }

    DirEntry entry;
    while (true) {
        long n = directory_read(static_cast<uint32_t>(dir), &entry);
        if (n < 0) {
            neutrino_write_line(out, "directory read failed");
            directory_close(static_cast<uint32_t>(dir));
            return 1;
        }
        if (n == 0) {
            break;
        }

        if ((entry.flags & DIR_ENTRY_FLAG_DIRECTORY) != 0) {
            neutrino_write(out, "[dir] ");
        } else {
            neutrino_write(out, "      ");
        }
        neutrino_write_line(out, entry.name);
    }

    directory_close(static_cast<uint32_t>(dir));
    return 0;
}
```

Directory programs should be prepared for entries to appear in filesystem
order. Sort only if your program explicitly needs sorted output and has a memory
plan for it.

## A Complete Example: Launcher

```cpp
#include <stdint.h>
#include <neutrino.h>
#include "syscall.hpp"

int main(uint64_t arg_ptr, uint64_t) {
    const char* command = reinterpret_cast<const char*>(arg_ptr);
    long out = neutrino_open_stdout();

    if (command == nullptr || command[0] == '\0') {
        neutrino_write_line(out, "usage: launch <program>");
        return 1;
    }

    long pid = child(command, "", 0, nullptr);
    if (pid < 0) {
        neutrino_write(out, "launch failed: ");
        neutrino_write_line(out, command);
        return 1;
    }

    neutrino_write_line(out, "launched");
    return 0;
}
```

This intentionally does not wait for the child to exit; the current syscall set
does not present a general `waitpid`-style interface in the template. Design
supervisors around the process and descriptor mechanisms that exist.

## System-Wide Helpers

`<neutrino.h>` also provides:

- `neutrino_get_time(out_time)`
- `neutrino_sync()`
- `neutrino_shutdown()`

Use `neutrino_sync()` for a command whose purpose is to flush filesystem/block
cache state. Use `neutrino_shutdown()` for administrative shutdown tools.

## Debugging Habits

Freestanding programs are easiest to debug when they are boring in the right
ways:

- Print usage before doing work.
- Print the operation and object when a syscall fails.
- Check every handle returned by an open call.
- Close every handle you successfully open.
- Keep buffers initialized when the following code expects strings.
- Avoid silently truncating paths or user input.
- Add small test modes before adding large interactive flows.
- Prefer one syscall wrapper per line while debugging.

When something fails early, verify that the program is being launched with the
argument string you think it is receiving. Many "filesystem" bugs are actually
empty or misparsed arguments.

## Porting Small Unix Programs

When porting a small Unix-style utility, translate concepts rather than calls:

- Replace `argc`/`argv` with parsing `arg_ptr`.
- Replace `printf` with fixed-buffer formatting plus `neutrino_write_line()`.
- Replace `open/read/write/close` with file syscalls for files.
- Replace writes to file descriptor `1` with `neutrino_open_stdout()` and
  `descriptor_write()`.
- Replace `opendir/readdir/closedir` with directory syscalls.
- Replace `sleep` with `sleep_seconds()` or `sleep_ms()`.
- Replace `fork/exec` designs with `child()` or `exec()`.
- Replace `mmap` with `map_anonymous()`, `map_at()`, or descriptor-specific
  mapped memory.

The result will usually be smaller and more explicit than the original.

## Where to Look Next

The best references are existing programs in the main Neutrino monorepo's
`userspace/programs` directory:

- `echo.cpp` for minimal output.
- `cat.cpp` for file reads and descriptor writes.
- `cp.cpp` for file copying.
- `ls.cpp` for directory iteration.
- `mkdir.cpp`, `rmdir.cpp`, `rm.cpp`, `touch.cpp`, and `write.cpp` for small
  filesystem utilities.
- `sleep.cpp`, `date.cpp`, `sync.cpp`, and `shutdown.cpp` for simple syscall
  wrappers.
- `shell.cpp` for process spawning and stdio handling.
- `top.cpp` and `dmesg.cpp` for monitoring and log descriptors.
- `netctl.cpp`, `tcpd.cpp`, and `dhcp.cpp` for network descriptors.
- `sheet.cpp` and `hexedit.cpp` for larger interactive programs.

Read those programs with the template headers open beside them. The headers
tell you what the ABI can do; the programs show the house style for making a
useful tool out of it.
