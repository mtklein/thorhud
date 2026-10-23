# thorhud

A quiet performance dashboard for the bottom screen of the AYN Thor running
[Armada OS](https://armadaos.dev/) — the Linux stand-in for the stock Android
overlay.

- **Header:** FPS (from gamescope's stats pipe), the focused game's title, battery % and time left/to full
- **Graph:** about a minute of FPS history on a fixed 0–120 scale (the display's top refresh rate), with reference lines at 30, 60 and 120
- **Meters:** CPU %, GPU % (DRM fdinfo) and clock, watts (USB in or battery draw), hottest CPU/GPU temperature + fan RPM, RAM
- **Knobs:** performance profile and GPU clock (auto or pinned at max), via Armada's `org.armada.Power` D-Bus service — no root needed.
  Each profile carries a CPU governor, which the HUD reasserts whenever the profile changes (including from Steam):
  Eco → conservative, Balanced → schedutil, Performance → performance

Colours are muted and equal in lightness; only temperature, battery draw and RAM (blue → amber → red)
change colour with their value; the battery ring runs green (≥40%), yellow (≥15%), red (≥5%), then bright purple.

## Build

Written in C against GTK 4. Armada's image has no compiler, so build in a
Fedora toolbox (same Fedora release as Armada, so the binary runs on the host):

```bash
toolbox create --distro fedora --release 44
toolbox run -c fedora-toolbox-44 sudo dnf install -y gcc make pkgconf-pkg-config gtk4-devel
toolbox run -c fedora-toolbox-44 make
```

## Install

On the Thor (user `armada`), from the repo:

```bash
cp thor-hud ~/.local/bin/
cp thor-hud.service ~/.config/systemd/user/
cp thor-hud.desktop ~/.local/share/applications/
systemctl --user daemon-reload && systemctl --user enable --now thor-hud.service
```

The service is tied to `armada-bottom-screen.service`, so it starts whenever the
bottom screen's Plasma session does, pinned to the little cores (0–2) at nice 10.
If you close it, "Thor HUD" in the bottom screen's app drawer brings it back.

## Testing

`thor-hud --windowed` runs in a 620×540 window; `thor-hud --snapshot out.png`
renders to a PNG after a few seconds and exits.
