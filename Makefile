CC := gcc
CFLAGS := -Wall -g -fPIC -lm
SOFLAGS := -shared
LDFLAGS := -ldl

all: server client libzerocopy.so

server: server_bench.c
	$(CC) $(CFLAGS) server_bench.c -o server

client: client_bench.c
	$(CC) $(CFLAGS) client_bench.c -o client -lm

libzerocopy.so: preload.c
	$(CC) $(SOFLAGS) $(CFLAGS) preload.c $(LDFLAGS) -o libzerocopy.so

clean:
	rm -f server client libzerocopy.so

.PHONY: all clean