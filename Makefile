CFLAGS+=`pkg-config --cflags jack` -Wall -g
LOADLIBES=`pkg-config --libs jack` -lm

jmtcdump: jmtcdump.c
