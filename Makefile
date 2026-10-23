CFLAGS ?= -O2 -g -Wall -Wextra
PKGS = gtk4

thor-hud: thor-hud.c
	$(CC) $(CFLAGS) -o $@ $< $$(pkg-config --cflags --libs $(PKGS)) -lm

clean:
	rm -f thor-hud
