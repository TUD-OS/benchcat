# -*- Mode: Makefile -*-

.PHONY: all clean
all: benchcat
clean:
	rm -f benchcat *.o *~

CFLAGS=-std=c99 -O2 -march=native -Wall -Wextra -D_GNU_SOURCE
LDFLAGS=-pthread

benchcat: benchcat.o

# EOF
