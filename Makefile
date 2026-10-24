CFLAGS ?= -O2 -g -Wall -Wextra
PKGS = gio-2.0 gio-unix-2.0 wayland-client freetype2 fontconfig zlib
XDG_SHELL = $(shell pkg-config --variable=pkgdatadir wayland-protocols)/stable/xdg-shell/xdg-shell.xml

.PHONY: dist clean

thor-hud: thor-hud.c xdg-shell-protocol.c xdg-shell-client-protocol.h
	$(CC) $(CFLAGS) -I. -o $@ thor-hud.c xdg-shell-protocol.c $$(pkg-config --cflags --libs $(PKGS)) -lm

xdg-shell-client-protocol.h:
	wayland-scanner client-header $(XDG_SHELL) $@

xdg-shell-protocol.c:
	wayland-scanner private-code $(XDG_SHELL) $@

# Release tarball: stripped binary plus everything install.sh needs.
DIST = thor-hud-aarch64
dist: thor-hud
	rm -rf $(DIST) $(DIST).tar.gz
	mkdir $(DIST)
	strip -o $(DIST)/thor-hud thor-hud
	cp install.sh thor-hud.service thor-hud.desktop README.md LICENSE $(DIST)/
	tar -czf $(DIST).tar.gz $(DIST)
	rm -rf $(DIST)

clean:
	rm -rf thor-hud xdg-shell-protocol.c xdg-shell-client-protocol.h $(DIST) $(DIST).tar.gz
