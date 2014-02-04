all: tojblockd

DBG=-g
CXXFLAGS=-W -Wall -O0 $(DBG)
CFLAGS=-W -Wall -O2 $(DBG)

tojblockd.o: vfat.h nbd.h fat.h
vfat.o: vfat.h ConvertUTF.h
ConvertUTF.o: ConvertUTF.h
fat.o: fat.h
dir.o: dir.h

tojblockd: tojblockd.o vfat.o ConvertUTF.o sd_notify.o fat.o dir.o
	$(CXX) $^ -o $@

.PHONY: clean

clean:
	rm -f tojblockd *.o
