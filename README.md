# thorhud

A quiet performance dashboard for the bottom screen of the AYN Thor running
[Armada OS](https://armadaos.dev/) — the Linux stand-in for the stock Android
overlay.

- **Header:** FPS (from gamescope's stats pipe), the focused game's title, battery % and time left/to full
- **Graph:** about a minute of FPS history, scaled up to the display's top refresh rate
- **Meters:** CPU %, GPU % (DRM fdinfo) and clock, watts (USB in or battery draw), hottest CPU/GPU temperature + fan RPM, RAM
- **Knobs:** performance profile and GPU clock (auto or pinned at max), via Armada's `org.armada.Power` D-Bus service — no root needed.
  Each profile carries a CPU governor, which the HUD reasserts whenever the profile changes (including from Steam):
  Eco → conservative, Balanced → schedutil, Performance → performance

Colours are muted and equal in lightness; only temperature and battery draw (blue → amber → red)
and low battery change colour with their value.

## Install

Copy the files to the Thor (user `armada`):

```bash
scp thor-hud armada@thor.local:.local/bin/
scp thor-hud.service armada@thor.local:.config/systemd/user/
scp thor-hud.desktop armada@thor.local:.local/share/applications/
ssh armada@thor.local 'systemctl --user daemon-reload && systemctl --user enable --now thor-hud.service'
```

The service is tied to `armada-bottom-screen.service`, so it starts whenever the
bottom screen's Plasma session does. If you close it, "Thor HUD" in the bottom
screen's app drawer brings it back.

Needs only what Armada ships: Python 3, PyGObject, GTK 4.

## Testing

`thor-hud --windowed` runs in a 620×540 window; `thor-hud --snapshot out.png`
renders to a PNG after a few seconds and exits.
