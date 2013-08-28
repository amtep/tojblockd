all: tojblockd

CFLAGS=-W -Wall -O2

tojblockd.o: vfat.h nbd.h
vfat.o: vfat.h sectorspace.h
sectorspace.o: vfat.h sectorspace.h

tojblockd: tojblockd.o vfat.o sectorspace.o

.PHONY: clean

clean:
	rm -f tojblockd *.o
