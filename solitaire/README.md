# Solitaire

A small standalone Klondike solitaire TUI for Neutrino.

## Build

```sh
make -C ../../neutrino/userspace newlib-sdk
make
make package
```

The binary is written to `out/solitaire.elf`. The package archive is written to
`out/solitaire.zip`.

Solitaire links against Neutrino's staged Newlib SDK. Its console and keyboard
access remains on the Neutrino descriptor API because those are OS-specific
devices.

## Play

Run `solitaire.elf` in Neutrino. You can play most of the game with single
keys:

- `d`: draw from the stock, or recycle the waste if the stock is empty.
- `w`: select the waste card, then press `1`-`7` or `f` to move it.
- `1`-`7`: select a tableau pile, or move the selected card/run there.
- `f` or `a`: move the selected card to a foundation, or auto-move one card if
  nothing is selected.
- `u`: undo one move.
- `r`: restart with a fresh deal.
- `?` or `h`: show or hide in-game help.
- `Esc`: clear the current selection or typed command.
- `q`: quit.

You can still type commands at the prompt and press Enter. If the first letter
is also a quick key, start the command with `/` or `:`, such as `/draw` or
`:quit`. Commands accept both short and wordy forms:

- `help`, `h`, or `?`: show or hide in-game help.
- `draw` or `d`: draw from the stock, or recycle the waste if the stock is empty.
- `auto`, `foundation`, or `f`: move the first available top card to a foundation.
- `move waste 3` or `m w 3`: move the waste card to tableau pile 3.
- `move waste foundation`, `move foundation waste`, or `m w f`: move waste to foundation.
- `move 4 foundation` or `m 4 f`: move tableau pile 4's top card to foundation.
- `move run 2 to 6` or `m 2 6`: move the longest legal face-up run from pile 2 to pile 6.
- `move 2 6 3` or `m 2 6 3`: move exactly three face-up cards from pile 2 to pile 6.
- `undo` or `u`: undo one move.
- `restart` or `r`: restart with a fresh deal.
- `quit` or `q`: quit.

Rules are standard draw-one Klondike: tableau cards build down in alternating
colors, foundations build up by suit from ace to king, and empty tableau piles
accept kings.
