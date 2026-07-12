# ApexSwitcher

A minimal, keyboard-driven workspace and window switcher for X11, inspired by the original
[superswitcher](https://code.google.com/archive/p/superswitcher) by Nigel Tao.

## Why ApexSwitcher?

I used superswitcher for years and loved it. It made navigating workspaces and windows fast
and intuitive in a way that nothing else quite matched. Unfortunately, the project was
abandoned around 2013 — Nigel moved on to write his own window manager
([taowm](https://github.com/nigeltao/taowm)) — and superswitcher stopped receiving updates
and became increasingly difficult to build as its GTK/GNOME dependency stack moved on without it.

ApexSwitcher is a from-scratch rewrite with these goals:

- **No heavy dependencies.** Only Xlib, Xft, and Xinerama — nothing from GTK or GNOME.
- **Minimal and purpose-focused.** It does one thing: help you navigate workspaces, viewports,
  and windows quickly from the keyboard. No configuration framework, no plugin system.
- **Faithful to the original spirit.** CapsLock as the modifier key, a clean overlay UI,
  instant switching.
- **Extended where it makes sense.** Per-viewport tracking per workspace, a window switcher,
  and other features that felt like natural extensions of the original idea.

## Credits

ApexSwitcher would not exist without superswitcher. Full credit to:

- **Nigel Tao** — author of the original superswitcher
  - Source code (GitHub mirror): https://github.com/nigeltao/superswitcher
  - Original project: https://code.google.com/archive/p/superswitcher
  - Farewell post: https://blogs.gnome.org/nigeltao/2013/01/28/so-long-and-thanks-for-the-super-switching/

## Building

```
gcc -O2 -o apexswitcher apexswitcher.c -I/usr/include/freetype2 -I/usr/include/libpng16 -lX11 -lXft -lXinerama
```

## Usage

Hold **CapsLock** to open the workspace overlay. While held:

| Key | Action |
|-----|--------|
| Left / Right | Switch workspace |
| Up / Down | Switch viewport |
| 1–0 | Jump directly to workspace 1–10 |
| Shift+CapsLock | Open window switcher |

While the **window switcher** is open:

| Key | Action |
|-----|--------|
| Up / Down | Navigate window list |
| 1–0, F1–F12 | Jump directly to window by index |
| Release CapsLock | Activate selected window and close |

## Similar Tools

If ApexSwitcher doesn't suit your needs, here are some alternatives:

### X11 / Linux

- **[superswitcher](https://github.com/nigeltao/superswitcher)** — the original; requires GTK/GNOME, no longer maintained
- **[rofi](https://github.com/davatorium/rofi)** — powerful window switcher, application launcher, and dmenu replacement; actively maintained, supports both X11 and Wayland
- **[simpleswitcher](https://github.com/seanpringle/simpleswitcher)** — lightweight EWMH window switcher; the ancestor of rofi

### macOS

- **[HyperSwitcher](https://hyperswitcher.app/)** — modern Mac app and window switcher with fixed keyboard shortcuts and Quick Layout
- **[HyperSwitch](https://bahoom.com/hyperswitch)** — Mac Alt-Tab replacement with window preview thumbnails

### Windows

- **[Microsoft PowerToys](https://github.com/microsoft/PowerToys)** — Microsoft's official power-user utilities suite, including FancyZones (custom window snap layouts) and an Alt-Tab window cycle tool
- **[Virtual Desktop Grid Switcher](https://sourceforge.net/projects/virtual-desktop-grid-switcher/)** — grid-based virtual desktop switcher for Windows 10/11 with arrow-key navigation, similar in spirit to ApexSwitcher
- **[AppSwitcher](https://app-switcher.com/)** — free Windows utility for assigning hotkeys to apps and switching between them instantly
