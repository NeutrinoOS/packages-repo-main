# Desktop

This package provides Neutrino's graphical desktop session. Appearance
preferences are stored in the signed-in user's settings hive under `desktop.*`
keys (for example, `desktop.background` and `desktop.cursor_bitmap`), so one
user's choices do not affect another user's session.

Launchers appear only in the Start menu. They are discovered at startup from
`/config/desktop/launchers/`, where every regular file is one launcher. Each
valid launcher needs a `label` and `path`; `args` is optional. For example:

```ini
label=Terminal
path=@sys/binary/terminal.elf
args=
```

Add, edit, or remove launcher files to customize the menu. The desktop loads
up to ten entries and ignores malformed files.

The taskbar includes a system-volume control. Click `VOL` to mute or unmute,
or click the slider to choose a level. Standard extended PS/2 media-key
scancodes for mute, volume down, and volume up are handled globally.

Open applications appear as task buttons between Start and the volume control.
Click one to bring it forward; `Alt+Tab` cycles focus through open windows.

Bitmap assets that should be installed with the desktop belong in
`package/share/desktop/bitmaps/`. They are packaged under
`/share/desktop/bitmaps` and can be addressed at runtime through
`@sys/share/desktop/bitmaps/`.

The `desktop.cursor_bitmap` setting selects a cursor image and defaults to
`@sys/share/desktop/bitmaps/cur.bmp`. Cursor images must be
uncompressed 16-bit A1R5G5B5 BMP files no larger than 64×64 pixels. The desktop
uses the one-bit alpha channel for transparency.

Cursor scaling is selected by display height and can be customized with
`desktop.cursor_scale_low`, `desktop.cursor_scale_720p`,
`desktop.cursor_scale_2k`, and `desktop.cursor_scale_4k`. Their defaults are 1× below 720p, 2× from 720p, 3× from
1440p, and 4× from 2160p. Configured factors must be integers from 1 through 8.
