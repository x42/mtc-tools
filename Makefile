PREFIX ?= /usr/local
bindir = $(PREFIX)/bin
mandir = $(PREFIX)/share/man/man1
CFLAGS ?= -Wall -g -O2

VERSION=0.1.0
targets= jmtcdump

ifeq ($(shell pkg-config --exists jack || echo no), no)
  $(error "http://jackaudio.org is required - install libjack-dev or libjack-jackd2-dev")
endif

ifeq ($(shell pkg-config --atleast-version=0.1.0 timecode || echo no), no)
  $(warning "MTC generator needs libtimcode >= 0.1.0 -- https://github.com/x42/libtimecode")
  $(warning "jmtcgen will not be built")
else
  targets+= jmtcgen
  CFLAGS+=`pkg-config --cflags timecode`
  LOADLIBES+=`pkg-config --libs timecode`
endif

ifeq ($(shell pkg-config --exists ltc || echo no), no)
  $(warning "MTC/LTC sync-test tool needs libtltc -- https://github.com/x42/libltc")
  $(warning "jmltcdebug will not be built")
else
  targets += jmltcdebug
  CFLAGS+=`pkg-config --cflags ltc`
  LOADLIBES+=`pkg-config --libs ltc` -lm
endif

CFLAGS+=`pkg-config --cflags jack` -DVERSION=\"$(VERSION)\" -pthread
LOADLIBES+=`pkg-config --libs jack` -lm

all: $(targets)

man: jmtcgen.1 jmtcdump.1

jmtcdump: jmtcdump.c

jmtcgen: jmtcgen.c

jmltcdebug: jmltcdebug.c

clean:
	rm -f jmtcgen jmtcdump jmltcdebug

jmtcgen.1: jmtcgen
	help2man -N -n 'JACK Transport to MTC' -o jmtcgen.1 ./jmtcgen

jmtcdump.1: jmtcdump
	help2man -N -n 'JACK MTC decoder' -o jmtcdump.1 ./jmtcdump

install: install-bin install-man

uninstall: uninstall-bin uninstall-man

install-bin: jmtcgen jmtcdump
	install -d $(DESTDIR)$(bindir)
	install -m755 jmtcgen $(DESTDIR)$(bindir)
	install -m755 jmtcdump $(DESTDIR)$(bindir)

uninstall-bin:
	rm -f $(DESTDIR)$(bindir)/jmtcgen
	rm -f $(DESTDIR)$(bindir)/jmtcdump
	-rmdir $(DESTDIR)$(bindir)

install-man:
	install -d $(DESTDIR)$(mandir)
	install -m644 jmtcgen.1 $(DESTDIR)$(mandir)
	install -m644 jmtcdump.1 $(DESTDIR)$(mandir)

uninstall-man:
	rm -f $(DESTDIR)$(mandir)/jmtcgen.1
	rm -f $(DESTDIR)$(mandir)/jmtcdump.1
	-rmdir $(DESTDIR)$(mandir)

.PHONY: all clean install uninstall man install-man install-bin uninstall-man uninstall-bin
