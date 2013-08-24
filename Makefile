all: tojblockd

CFLAGS=-W -Wall -O2

tojblockd.o: udf.h nbd.h
udf.o: udf.h

tojblockd: tojblockd.o udf.o

.PHONY: clean

clean:
	rm -f tojblockd *.o
