# peanutbutty — Makefile
# Targets PowerPC 32-bit Linux (and any other POSIX/X11/OpenGL 2.0 host)

CC      ?= gcc
TARGET  ?= peanutbutty

# Core flags
CFLAGS  = -std=c99 -O3 -Wall -Wextra -Wno-unused-parameter
CFLAGS += -D_XOPEN_SOURCE=700 -D_GNU_SOURCE

# Dependency flags
PKG_CFLAGS = $(shell pkg-config --cflags freetype2 fontconfig x11 gl)
LIBS       = $(shell pkg-config --libs freetype2 fontconfig x11 gl) -lm -lutil

# Arch detection
ARCH := $(shell uname -m)

# PowerPC 32-bit (G4 / 74xx) optimization flags
ifneq (,$(filter $(ARCH),ppc powerpc ppc64))
    PPC_OPT = -mcpu=7450 -mtune=7450 -maltivec -mabi=altivec -ffast-math -fexpensive-optimizations
else
    PPC_OPT = -O3
endif

SRC     = peanutbutty.c
OBJ     = $(SRC:.c=.o)

.PHONY: all clean install native ppc32

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) $(OPT) -o $@ $^ $(LIBS)

%.o: %.c
	$(CC) $(CFLAGS) $(OPT) $(PKG_CFLAGS) -c -o $@ $<

native:
	$(MAKE) OPT="-O3"

ppc32:
	$(MAKE) OPT="$(PPC_OPT)"

clean:
	rm -f $(OBJ) $(TARGET)

install: $(TARGET)
	install -Dm755 $(TARGET) $(DESTDIR)/usr/local/bin/$(TARGET)
