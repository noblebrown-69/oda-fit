CC = /usr/bin/gcc
CFLAGS = -std=c11 -Wall -Wextra -O2
LIBS = -lsqlite3

.PHONY: all clean

all: fitd

fitd: fitd.c
	$(CC) $(CFLAGS) -o $@ fitd.c $(LIBS)

clean:
	rm -f fitd
