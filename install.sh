#!/bin/sh
# Installs thor-hud for the current user on an AYN Thor running Armada OS.
# No root needed: everything goes under ~/.local and ~/.config.
set -eu
cd "$(dirname "$0")"

# Replace the binary by rename, so it works while the old one is running.
mkdir -p ~/.local/bin ~/.config/systemd/user ~/.local/share/applications
install -m755 thor-hud ~/.local/bin/thor-hud.new
mv -f ~/.local/bin/thor-hud.new ~/.local/bin/thor-hud
install -m644 thor-hud.service ~/.config/systemd/user/thor-hud.service
install -m644 thor-hud.desktop ~/.local/share/applications/thor-hud.desktop

systemctl --user daemon-reload
systemctl --user enable thor-hud.service
# Starts only while the bottom screen's session is up; otherwise it starts with it.
systemctl --user restart thor-hud.service 2>/dev/null || true

echo "thor-hud installed."
if [ -e /etc/armada/bottom-screen-brightness ]; then
    echo "For bottom-screen brightness swipes to stick, move Armada's saved setting aside:"
    echo "  sudo mv /etc/armada/bottom-screen-brightness /etc/armada/bottom-screen-brightness.bak"
fi
if [ ! -e /etc/inputplumber/capability_maps.d/ayn_type2.yaml ]; then
    echo "For the AYN button to turn the bottom screen off and on, route it to thor-hud:"
    echo "  sudo $(pwd)/ayn-button.sh"
fi
