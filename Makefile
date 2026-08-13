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
OBJS = unvise.o extract.o io.o init.o inflate.o catalog.o

.PHONY: all clean install

all: unvise

unvise: $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

$(OBJS): unvise.h

install: unvise
	$(MKDIR_P) $(DESTDIR)$(BINDIR)
	$(INSTALL) -m 755 unvise $(DESTDIR)$(BINDIR)/unvise

clean:
	$(RM) unvise $(OBJS)
