all: tojblockd

CFLAGS=-W -Wall -O2

tojblockd.o: udf.h nbd.h
udf.o: udf.h sectorspace.h
sectorspace.o: udf.h sectorspace.h

tojblockd: tojblockd.o udf.o sectorspace.o

.PHONY: clean

clean:
	rm -f tojblockd *.o
