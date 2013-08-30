# -*- Mode: Makefile -*-

.PHONY: all clean
all: benchcat benchcat.cpio
clean:
	rm -f benchcat benchcat.cpio *.o *~

CFLAGS=-std=c11 -O2 -march=native -Wall -Wextra -D_GNU_SOURCE
LDFLAGS=-pthread -static

benchcat: benchcat.o

benchcat.cpio: benchcat
	mkdir -p usr/bin
	cp benchcat usr/bin
	strip usr/bin/benchcat
	find usr | cpio -oH newc > $@
	rm -r usr

# EOF
