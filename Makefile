# Makefile - telnet's stand-in build for a Mac (tests: see wsim.c).
# On the 3B1 itself: sh mk3b1

CC = cc
FONTDIR = /usr/lib/wfont/
CFLAGS = -O -g -Wall -Wno-unused-parameter -Wno-pointer-sign \
	-DFONTDIR=\"$(FONTDIR)\"
SRC = telnet.c vt.c glyph.c ttyb.c net.c

tnsim: $(SRC) wsim.c tn.h syms.h
	$(CC) $(CFLAGS) -o tnsim $(SRC) wsim.c -lz

syms.h: syms.txt tools/mksyms.py
	python3 tools/mksyms.py syms.txt > syms.h

clean:
	rm -f tnsim *.o
