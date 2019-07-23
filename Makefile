CC=gcc
CFLAGS=-lwayland-client

demo:
	$(CC) $(CFLAGS) demo.c xdg-shell-unstable-v6-client-protocol.h xdg-shell-unstable-v6-client-protocol.c -o demo

clean:
	rm demo
