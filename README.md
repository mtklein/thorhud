# thorhud

A quiet performance dashboard for the bottom screen of the AYN Thor running
[Armada OS](https://armadaos.dev/) — the Linux stand-in for the stock Android
overlay.

<img src="docs/screenshot.png" width="620" alt="thor-hud on the Thor's bottom screen: FPS, game title and battery across the top, a one-minute FPS graph, the profile buttons, and temperature, CPU, GPU, RAM and watts meters">

- **Header:** FPS (from gamescope's stats pipe), the focused game's title, battery % and time left/to full
- **Meters:** CPU %, GPU % (DRM fdinfo) and clock, watts (USB in or battery draw), hottest CPU/GPU temperature + fan RPM, RAM
- **Profile:** Eco / Balanced / Performance via Armada's `org.armada.Power` D-Bus service — no root needed.
  Each profile carries a CPU governor, which the HUD reasserts whenever the profile changes (including from Steam):
  Eco → conservative, Balanced → schedutil, Performance → performance
- **Graphs:** tap any stat — the FPS readout, the battery corner, or a gauge — to graph it; tap it again to
  leave the middle empty. The choice is remembered across restarts. FPS, CPU, GPU and
  watts show the last 2 minutes, temperature and RAM 5, battery 2 hours; noisy stats are drawn as a short
  moving average over a faint fill of the raw readings, with a tick marking the midpoint of the time axis.
- **Brightness swipes:** drag up/down anywhere — left half for the bottom screen, right half for the top. Any
  touch that moves more than a few pixels is a swipe, even one starting on a button; only taps that stay put
  press buttons or pick graphs. It goes through logind's `Session.SetBrightness`. Armada re-applies its own saved bottom brightness from
  `/etc/armada/bottom-screen-brightness` every 2 s, so bottom swipes only stick once that file is removed
  (`sudo mv /etc/armada/bottom-screen-brightness /etc/armada/bottom-screen-brightness.bak`); the HUD then keeps
  its own setting in `~/.local/state/thor-hud/bottom-brightness`.

Colours are muted and equal in lightness; only temperature, battery draw and RAM (blue → amber → red)
change colour with their value; the battery ring is blue when full, then green (≥40%), yellow (≥15%), red (≥5%), and bright purple.

## Install

On the Thor (Armada OS, user `armada`), from a terminal or over SSH:

```bash
curl -L https://github.com/mtklein/thorhud/releases/latest/download/thor-hud-aarch64.tar.gz | tar xz
./thor-hud-aarch64/install.sh
```

It installs into `~/.local` and `~/.config` (no root), enables the service and
starts it. The service is tied to `armada-bottom-screen.service`, so it starts
whenever the bottom screen's Plasma session does, pinned to the little cores
(0–2) at nice 10. If you close it, "Thor HUD" in the bottom screen's app drawer
brings it back.

## Build

A plain Wayland client in C: it rasterises its own pixels (distance-field arcs,
lines and rounded boxes; FreeType glyphs cached once) into shared-memory
buffers, sends only the regions that changed, and handles touch itself. GLib
provides the event loop and D-Bus. It uses about 1.3% of one little core.

Armada's image has no compiler, so build in a Fedora toolbox (same Fedora
release as Armada, so the binary runs on the host):

```bash
toolbox create --distro fedora --release 44
toolbox run -c fedora-toolbox-44 sudo dnf install -y gcc make pkgconf-pkg-config \
    wayland-devel wayland-protocols-devel freetype-devel fontconfig-devel glib2-devel zlib-ng-compat-devel
toolbox run -c fedora-toolbox-44 make
```

## Install from source

On the Thor (user `armada`), from the repo after building:

```bash
./install.sh
```

`make dist` builds the release tarball (`thor-hud-aarch64.tar.gz`): a stripped
binary plus `install.sh`, the service and launcher files.

## Testing

`thor-hud --windowed` runs in a 620×540 window; `thor-hud --snapshot out.png`
writes its own pixels to a PNG after a few seconds and exits; `thor-hud --stats`
prints the CPU time per tick spent sampling and drawing; `--show cpu` (or fps, temp,
gpu, ram, watts, battery, none) picks the graph at startup without saving it; `--snapshot-delay 60` waits
longer before the snapshot.
