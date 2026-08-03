# SDL2 for Neutrino

This package builds SDL2 with native Neutrino drivers and installs both the
shared/development libraries and public headers.

The video driver first connects to the Neutrino desktop and presents an
ARGB8888 shared-memory window. When no desktop is running, it acquires a
graphical session and scales the software framebuffer directly to the active
display. Keyboard events work in both modes. The audio driver writes 48 kHz,
stereo, signed 16-bit PCM to the HDA audio descriptor.

Build and package it with:

```sh
make package
```

The resulting `out/sdl2.zip` is version `0.1.1`. The staged development root
used by sibling cross-builds is `out/usr`; `tools/sdl2-config-cross` reads that
location from `SDL2_ROOT`.

This initial port provides software rendering, keyboard input, and default
audio output. Mouse, joystick, capture audio, OpenGL, and Vulkan are not yet
implemented.
