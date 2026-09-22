# PeanutBuTTY

A minimal, OpenGL 2.0 accelerated terminal emulator for X11.
Targets PowerPC 32-bit Linux (PowerBook G4 / Mobility Radeon 9600 with Mesa r300).
Inspired by Alacritty's clean aesthetic.

## Features

- OpenGL 2.0 / GLSL 1.10 rendering (compatible with older Mesa/DRI drivers)
- VT100 / VT220 / xterm-256color emulation
- Full SGR: real bold / italic / bold-italic faces (synthesised when the
  family has none), underline / double underline / undercurl (`CSI 4:2m`,
  `4:3m`) with SGR 58 underline colours, strikethrough, blink, dim,
  invisible, reverse; `:`-subparameter colour forms (38:2::R:G:B)
- 256-colour palette + 24-bit truecolor; dynamic colours via OSC 4/10/11/12
  (set + query + reset)
- UTF-8, wide (CJK) characters, fontconfig font fallback for missing glyphs
- **Procedural box drawing**: U+2500-257F box/line characters (light,
  heavy, double, dashed), U+2580-259F block elements/shades/quadrants and
  powerline triangles (U+E0B0/E0B2) are drawn as exact-cell-geometry quads
  instead of font glyphs — tmux borders and TUI frames are seamless at any
  font size
- **Reflow**: soft-wrapped lines re-wrap to the new width on resize
  (debounced so interactive resizes stay smooth); WM size hints snap
  resizes to the cell grid
- Config file `~/.config/peanutbutty.conf` (font, size, colours, padding,
  scrollback, blink) — see `peanutbutty.conf.example`
- Runtime font size: Ctrl+Plus / Minus / 0 (Shift optional), Ctrl+wheel
- Window padding; DECSCUSR cursor shapes (block / underline / bar,
  blink or steady), hollow cursor when unfocused
- Scrollback (5 000 lines default) — Shift+PageUp / PageDown, mouse wheel
- Copy (Ctrl+Shift+C) / Paste (Ctrl+Shift+V), PRIMARY + middle-click,
  double-click word / triple-click line select, UTF-8 + TARGETS protocol
- **OSC 52** clipboard set/query — remote vim/tmux over SSH can copy into
  the local clipboard
- Bracketed paste, focus events, synchronized output (mode 2026, with
  stuck-update timeout)
- **DECRQM** mode queries (`CSI ? Ps $ p`) — apps can detect 2026/2004/1006
  etc. before enabling them
- **Kitty keyboard protocol** level 1 (disambiguate escape codes) with
  push/pop/query flag stack; Esc and Ctrl/Alt-modified keys get unambiguous
  `CSI u` encodings
- **XTGETTCAP** replies for `TN`, `Co`/`colors`, `RGB` — truecolor
  auto-detection without `COLORTERM` guessing
- **Scrollback search** (Ctrl+Shift+F): incremental, case-insensitive,
  match highlighting; Enter/Up = older, Down = newer, Esc closes
- **Shell integration** (OSC 133 prompt marks): Ctrl+Shift+Up / Down jump
  between prompts in the scrollback
- **Keyboard URL hints** (Ctrl+Shift+U): every visible URL gets a one-key
  label, no trackpad needed
- **Live config reload**: edits to `peanutbutty.conf` apply on save
  (colours, palette, font size, padding, blink); `SIGUSR1` also triggers a
  reload; font family and scrollback depth still need a restart
- **XIM input method** support (ibus/fcitx, dead keys) via
  `Xutf8LookupString`, falling back to core lookup
- **OSC 7** working-directory tracking (stored for future tab/window
  inheritance)
- Mouse reporting: X10 / normal / button-motion / any-motion (1000/1002/1003),
  SGR extended coords (1006); Shift overrides for local selection
- Wheel scrolls scrollback on the main screen, sends arrows on the alt screen
- Alternate screen, origin mode (DECOM), scroll regions, RIS
- Sixel graphics: colour definitions, repeats, transparency, cropping
- OSC 0/2 window title, OSC 8 hyperlinks + plain-text URL detection
  (Ctrl+click opens via xdg-open)
- Visual bell (border flash) + WM urgency hint when unfocused
- WM_CLASS set for window-manager rules and taskbar grouping
- **Performance**: renders only when content, blink, or focus changes;
  frame-paced at ~60 fps under load; sleeps indefinitely when idle
  (~0.3 % CPU with a blinking cursor, ~9 % under a 20 fps full-screen
  ncurses app on the 1.67 GHz G4); clean exit on shell exit / window close
- GL context recreation after DPMS/blank (CPU shadow of the glyph atlas)
- Single C99 source file; libX11, libGL, freetype2, fontconfig, libutil, libm

## Build

```sh
make ppc32     # on the G4: -mcpu=7450 -maltivec etc.
make native    # everywhere else
./peanutbutty
sudo make install PREFIX=/usr/local   # binary, .desktop, icon, docs
```

Arch Linux (any arch, including Arch POWER on the G4): `cd pkg && makepkg -si`
builds and installs the `peanutbutty-git` package from this repository.

### Dependencies (runtime)

| Library    | Package (Arch)   |
|------------|------------------|
| libX11     | `libx11`         |
| libGL      | `mesa`           |
| freetype2  | `freetype2`      |
| fontconfig | `fontconfig`     |
| libutil    | part of glibc    |

Mesa's `r300` / `r600` / `softpipe` drivers all expose OpenGL 2.0 and work on
big-endian PPC. The shaders use GLSL 1.10 (`#version 110`), the oldest
portable GLSL version.

## Configuration

Command line: `-w <px> -h <px>` initial window size (default 1024×768).

Runtime config in `~/.config/peanutbutty.conf` (`key = value`, `#` comments) —
copy `peanutbutty.conf.example` to start. Keys: `font`, `size`, `padding`,
`scrollback`, `blink`, `foreground`, `background`, `cursor`, `color0`-`color15`.
The default palette is Tomorrow Night.

## Keyboard / mouse shortcuts

| Input                       | Action                                    |
|-----------------------------|-------------------------------------------|
| Ctrl+Shift+C                | Copy selection to CLIPBOARD (and PRIMARY) |
| Ctrl+Shift+V                | Paste from CLIPBOARD                      |
| Ctrl+Plus / Minus / 0       | Grow / shrink / reset font size (Shift optional; keypad too) |
| Ctrl+wheel                  | Grow / shrink font size                   |
| Ctrl+Shift+F                | Search scrollback (Esc closes)            |
| Ctrl+Shift+U                | URL hint mode: label key opens the URL    |
| Ctrl+Shift+Up / Down        | Jump to previous / next prompt (OSC 133)  |
| Middle click                | Paste from PRIMARY                        |
| Double / triple click       | Select word / line                        |
| Shift+Page Up/Down          | Scroll back / forward one page            |
| Mouse wheel                 | Scroll back/forward 3 lines               |
| Shift+drag                  | Select even when the app grabs the mouse  |
| Ctrl+click URL              | Open in browser (xdg-open)                |

Any other keypress snaps the view back to the live screen.

## Architecture notes for PPC big-endian

- Vertex colors are packed `(r<<24)|(g<<16)|(b<<8)|a`: on big-endian this puts
  bytes `[r,g,b,a]` in memory, which the GPU reads per-byte for
  `GL_UNSIGNED_BYTE` attributes — no word swap occurs. Do not "fix" this.
- The glyph atlas uploads as `GL_LUMINANCE`/`GL_UNSIGNED_BYTE` —
  endian-neutral.
- Sixel RGBA packing `(rgb<<8)|0xFF` likewise relies on BE byte order.
- `openpty(3)` from glibc works on PPC; standard POSIX fds throughout.

## License

Public domain / CC0.  Do whatever you like.
