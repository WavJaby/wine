# wine-wayland fork

This is an independent fork of [Wine](https://www.winehq.org/) 11.18 (tag `wine-11.18`) for the [wine-wayland-desktop](https://github.com/WavJaby/wine-wayland-desktop) project.
It is not Wine and is not endorsed, supported or reviewed by the Wine project or WineHQ; do not report its problems to WineHQ.
Wine's own README, LICENSE (LGPL-2.1-or-later), COPYING.LIB, AUTHORS and NOTICES files are unchanged.

The fork adds window, DPI, touch, tray, notification and file-dialog behaviour for the native Wayland driver (`winewayland.drv`).
Branch `wine-wayland/11.18` is the supported line; the diff against `wine-11.18` is the full change set.

## Third-party work

Parts of the fork come from other people's Wine merge requests; each author keeps their copyright.

| Fork path | Source | Author |
|---|---|---|
| `dlls/win32u/snidrv/` | !2808 SNI tray implementation | Sergei Chernyadyev |
| `dlls/comdlg32/` portal backend, portal parts of `dlls/shell32/` and `programs/winecfg/` | !10060 XDG desktop portal for native file dialogs | Alexander Wilms |
| `dlls/winewayland.drv/wayland_touch.c` | !11116 winewayland touch input | Etaash Mathamsetty |
| `dlls/wpnapps/` `main.c`, `wpnapps.spec`, `Makefile.in`, `classes.idl`, `private.h`; `include/notificationactivationcallback.idl` | !12256, !12433 wpnapps stub DLL | Vibhav Pant (CodeWeavers) |

MR URLs: `https://gitlab.winehq.org/wine/wine/-/merge_requests/<number>`.
Local changes to these files are in the git history.

`dlls/wpnapps/` `toast.c`, `notify.c`, `unixlib.c`, `unixlib.h` and `xml.c` were written with an AI coding assistant; no upstream counterpart was found.
The rest of the fork's changes were also made with AI assistance and tested by the maintainer.
Wine's contribution policy excludes AI-generated code, so these changes are not intended for submission to Wine.

Detailed attribution: [NOTICE.md](https://github.com/WavJaby/wine-wayland-desktop/blob/main/NOTICE.md) in the wine-wayland-desktop repository.
