PREFIX ?= /usr/local
bindir = $(PREFIX)/bin
mandir = $(PREFIX)/share/man/man1/

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

jmtcdump: jmtcdump.c

jmtcgen: jmtcgen.c

clean:
	rm -f jmtcgen jmtcdump

.PHONY: all clean
