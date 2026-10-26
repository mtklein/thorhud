#!/bin/sh
# Routes the Thor's AYN button to thor-hud, which uses it to turn the bottom
# screen off and on. Run once with sudo.
#
# InputPlumber maps the button (KEY_F24) to a gamepad Screenshot button the
# emulated controller doesn't have, so out of the box it does nothing. This
# installs a copy of InputPlumber's AYN capability map that sends it to
# InputPlumber's D-Bus device instead, as ui_quick2, where thor-hud listens.
#
#   sudo ./ayn-button.sh           install the override
#   sudo ./ayn-button.sh --remove  go back to InputPlumber's own map
#
# The override lives in /etc, which belongs to the current OS deployment:
# after a rollback, run this again.
set -eu
[ "$(id -u)" = 0 ] || { echo "run with sudo" >&2; exit 1; }
src=/usr/share/inputplumber/capability_maps/ayn_type2.yaml
dst=/etc/inputplumber/capability_maps.d/ayn_type2.yaml

if [ "${1:-}" = --remove ]; then
    rm -f "$dst"
else
    grep -q 'button: QuickAccess2' "$src" || { echo "unexpected $src; not changing anything" >&2; exit 1; }
    mkdir -p "$(dirname "$dst")"
    {
        echo "# Installed by thor-hud's ayn-button.sh: the AYN key goes to InputPlumber's"
        echo "# D-Bus device (ui_quick2) instead of the gamepad."
        sed '/name: AYN Key/,/button: QuickAccess2/{s/^      gamepad:$/      dbus: ui_quick2/;/button: QuickAccess2/d}' "$src"
    } >"$dst.new"
    mv "$dst.new" "$dst"
fi
systemctl restart inputplumber
