CFLAGS ?= -O2 -g -Wall -Wextra
PKGS = gio-2.0 gio-unix-2.0 wayland-client freetype2 fontconfig zlib
XDG_SHELL = $(shell pkg-config --variable=pkgdatadir wayland-protocols)/stable/xdg-shell/xdg-shell.xml

thor-hud: thor-hud.c xdg-shell-protocol.c xdg-shell-client-protocol.h
	$(CC) $(CFLAGS) -I. -o $@ thor-hud.c xdg-shell-protocol.c $$(pkg-config --cflags --libs $(PKGS)) -lm

xdg-shell-client-protocol.h:
	wayland-scanner client-header $(XDG_SHELL) $@

xdg-shell-protocol.c:
	wayland-scanner private-code $(XDG_SHELL) $@

clean:
	rm -f thor-hud xdg-shell-protocol.c xdg-shell-client-protocol.h
