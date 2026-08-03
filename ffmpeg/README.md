# FFmpeg for Neutrino

This package builds the FFmpeg, FFplay, and FFprobe command-line programs and
reusable shared libraries for Neutrino. The enabled surface includes:

- local files plus IPv4 TCP, HTTP, and HTTPS inputs;
- Matroska/WebM and MP4/MOV demuxing;
- H.264, VP8, and VP9 video decoding;
- AAC and MP3 audio decoding;
- MP4/MOV, ADTS, MP3, raw-video, and null output muxers;
- SDL2 software-rendered playback through the Neutrino desktop or raw
  framebuffer;
- pthread-based codec workers.

HTTPS uses BearSSL and verifies both the hostname and certificate chain. It
requires a valid system clock and the OS trust store at
`/config/ssl/cacert.pem`; HTTPS fails closed if either is unavailable.

The upstream FFmpeg checkout is under `src/`; its configure script has a small
cross-build fix so an explicitly supplied `SDL2_CONFIG` is honored.

## Build

Build the sibling Neutrino Newlib SDK first if it is not already available:

```sh
make -C ../../neutrino/userspace newlib-sdk
```

Build the sibling SDL2 and BearSSL packages, then build and validate FFmpeg:

```sh
make -C ../sdl2
make -C ../bearssl
make
make verify
```

The executables are written to `out/ffmpeg.elf`, `out/ffplay.elf`, and
`out/ffprobe.elf`. Shared objects, linker-name copies, pkg-config metadata, and
public headers are staged under `out/library` and `out/include`. Override
`NEUTRINO_ROOT` when the Neutrino repository is not at `../../neutrino`.

## Package

Create an installable Neutrino package with:

```sh
make package
```

This produces `out/ffmpeg.zip` with uncompressed entries and the standard
Neutrino package layout:

```text
manifest.toml
binary/ffmpeg.elf
binary/ffplay.elf
binary/ffprobe.elf
include/libavcodec/
include/libavformat/
include/libavutil/
library/libavcodec.so
library/libavcodec.so.63
config/
scripts/
```

The package also contains `libavdevice`, `libavfilter`, `libswresample`, and
`libswscale`, each with its ABI-major filename and unversioned linker name.
The tools depend on these packaged libraries, and `ffplay` additionally uses
the separately packaged SDL2 shared object. HTTP uses Neutrino's POSIX socket
compatibility layer; HTTPS additionally uses the separately packaged BearSSL
shared object and the trust store from `ca-certificates`. The package manifest
therefore depends on `sdl2`, `bearssl`, and `ca-certificates`.

The current POSIX network surface intentionally supports IPv4 TCP clients.
Listening sockets, IPv6, and general UDP sockets return normal unsupported
errors until those transports are implemented.

## Licensing

FFmpeg's license files are included with the upstream checkout under `src/`.
The current minimal configuration reports LGPL 2.1-or-later; enabling
additional components may change the resulting binary's licensing terms.
