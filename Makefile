# peanutbutty — Makefile
# Targets PowerPC 32-bit Linux (and any other POSIX/X11/OpenGL 2.0 host)

CC      ?= gcc
TARGET  ?= peanutbutty
PREFIX  ?= /usr/local

# Core flags
CFLAGS  = -std=c99 -O3 -Wall -Wextra -Wno-unused-parameter
CFLAGS += -D_XOPEN_SOURCE=700 -D_GNU_SOURCE

# Dependency flags
PKG_CFLAGS = $(shell pkg-config --cflags freetype2 fontconfig x11 gl)
LIBS       = $(shell pkg-config --libs freetype2 fontconfig x11 gl) -lm -lutil

# Arch detection
ARCH := $(shell uname -m)

# Per-arch optimization flags (used by the `ppc32` / `ppc64le` targets)
ifneq (,$(filter $(ARCH),ppc powerpc ppc64))
    # PowerPC 32-bit (G4 / 74xx)
    PPC_OPT = -mcpu=7450 -mtune=7450 -maltivec -mabi=altivec -ffast-math -fexpensive-optimizations
else ifeq ($(ARCH),ppc64le)
    # POWER8+ little-endian
    PPC_OPT = -O3 -mcpu=power8 -mtune=power8 -maltivec -mvsx
else
    PPC_OPT = -O3
endif

SRC     = peanutbutty.c
OBJ     = $(SRC:.c=.o)

.PHONY: all clean install native ppc32 ppc64le icon

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) $(OPT) $(LDFLAGS) -o $@ $^ $(LIBS)

%.o: %.c icon_data.h
	$(CC) $(CFLAGS) $(OPT) $(PKG_CFLAGS) -c -o $@ $<

native:
	$(MAKE) OPT="-O3"

ppc32:
	$(MAKE) OPT="$(PPC_OPT)"

ppc64le:
	$(MAKE) OPT="$(PPC_OPT)"

clean:
	rm -f $(OBJ) $(TARGET)

install: $(TARGET)
	install -Dm755 $(TARGET) $(DESTDIR)$(PREFIX)/bin/$(TARGET)
	install -Dm644 peanutbutty.desktop $(DESTDIR)$(PREFIX)/share/applications/peanutbutty.desktop
	install -Dm644 peanutbutty.png $(DESTDIR)$(PREFIX)/share/icons/hicolor/256x256/apps/peanutbutty.png
	install -Dm644 peanutbutty.conf.example $(DESTDIR)$(PREFIX)/share/doc/peanutbutty/peanutbutty.conf.example
	install -Dm644 README.md $(DESTDIR)$(PREFIX)/share/doc/peanutbutty/README.md

# Regenerate the embedded window icon + peanutbutty.png from peanutbuttyicon.jpg (needs python3 + Pillow)
icon:
	python3 gen_icon.py
