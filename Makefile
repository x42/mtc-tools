PREFIX ?= /usr/local
bindir = $(PREFIX)/bin
mandir = $(PREFIX)/share/man/man1

VERSION=0.0.1

ifeq ($(shell pkg-config --exists jack || echo no), no)
  $(error "http://jackaudio.org is required - install libjack-dev or libjack-jackd2-dev")
endif
ifeq ($(shell pkg-config --exists timecode || echo no), no)
  $(error "libtimcode is required - install https://github.com/x42/libtimecode")
endif

CFLAGS+=`pkg-config --cflags jack timecode` -DVERSION=\"$(VERSION)\" -Wall -g
LOADLIBES=`pkg-config --libs jack timecode` -lm

all: jmtcgen jmtcdump

man: jmtcgen.1 jmtcdump.1

jmtcdump: jmtcdump.c

jmtcgen: jmtcgen.c

clean:
	rm -f jmtcgen jmtcdump

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
	rm -f $(DESTDIR)$(mandir)/jltcdump.1
	rm -f $(DESTDIR)$(mandir)/jmtcdump.1
	-rmdir $(DESTDIR)$(mandir)

.PHONY: all clean install uninstall man install-man install-bin uninstall-man uninstall-bin
