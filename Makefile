CC ?= cc
CFLAGS ?= -O2
CFLAGS += -std=c99 -Wall -Wextra -Wpedantic
LDLIBS += -lz

PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
DESTDIR ?=
INSTALL ?= install
MKDIR_P ?= mkdir -p
RM ?= rm -f

.PHONY: all clean install

all: unvise

unvise: unvise.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ unvise.c $(LDFLAGS) $(LDLIBS)

install: unvise
	$(MKDIR_P) $(DESTDIR)$(BINDIR)
	$(INSTALL) -m 755 unvise $(DESTDIR)$(BINDIR)/unvise

clean:
	$(RM) unvise
