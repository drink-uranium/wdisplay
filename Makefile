PREFIX  ?= /usr/local
BINDIR  ?= $(PREFIX)/bin
MANDIR  ?= $(PREFIX)/share/man/man1

CC      ?= cc
CFLAGS  ?= -O2 -g -Wall -Wextra -Wno-unused-parameter
PKGS     = WINGs xrandr x11
CPPFLAGS += $(shell pkg-config --cflags $(PKGS))
LDLIBS  += $(shell pkg-config --libs $(PKGS)) -lm

SRCS = src/main.c src/display.c src/layout.c src/profile.c src/ui.c
OBJS = $(SRCS:.c=.o)

all: wdisplay

wdisplay: $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

src/ui.o: src/icon.xpm
$(OBJS): src/display.h src/layout.h src/profile.h src/ui.h

install: wdisplay
	install -d $(DESTDIR)$(BINDIR) $(DESTDIR)$(MANDIR)
	install -m 755 wdisplay $(DESTDIR)$(BINDIR)/wdisplay
	install -m 644 wdisplay.1 $(DESTDIR)$(MANDIR)/wdisplay.1

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/wdisplay $(DESTDIR)$(MANDIR)/wdisplay.1

clean:
	rm -f wdisplay $(OBJS)

.PHONY: all install uninstall clean
