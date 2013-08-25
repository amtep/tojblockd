all: tojblockd

CFLAGS=-W -Wall -O2

tojblockd.o: udf.h nbd.h
udf.o: udf.h sectorspace.h
sectorspace.o: udf.h sectorspace.h
crc.o: udf.h

tojblockd: tojblockd.o udf.o sectorspace.o crc.o

.PHONY: clean

clean:
	rm -f tojblockd *.o
