all: tojblockd

CFLAGS=-W -Wall -O2

tojblockd.o: vfat.h nbd.h
vfat.o: vfat.h

tojblockd: tojblockd.o vfat.o
	$(CXX) $^ -o $@

.PHONY: clean

clean:
	rm -f tojblockd *.o
