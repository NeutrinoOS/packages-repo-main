# Desktop

This package provides Neutrino's graphical desktop session and its appearance
and launcher configuration.

Bitmap assets that should be installed with the desktop belong in
`package/share/desktop/bitmaps/`. They are packaged under
`/share/desktop/bitmaps` and can be addressed at runtime through
`@sys/share/desktop/bitmaps/`.

The `cursor_bitmap` setting in `/config/desktop.cfg` selects a cursor image and
defaults to `@sys/share/desktop/bitmaps/cur.bmp`. Cursor images must be
uncompressed 16-bit A1R5G5B5 BMP files no larger than 64×64 pixels. The desktop
uses the one-bit alpha channel for transparency.

Cursor scaling is selected by display height and can be customized with
`cursor_scale_low`, `cursor_scale_720p`, `cursor_scale_2k`, and
`cursor_scale_4k`. Their defaults are 1× below 720p, 2× from 720p, 3× from
1440p, and 4× from 2160p. Configured factors must be integers from 1 through 8.
