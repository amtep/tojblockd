all: tojblockd

DBG=-g
CXXFLAGS=-W -Wall -O2 $(DBG)
CFLAGS=-W -Wall -O2 $(DBG)

tojblockd.o: vfat.h nbd.h
vfat.o: vfat.h ConvertUTF.h
ConvertUTF.o: ConvertUTF.h

tojblockd: tojblockd.o vfat.o ConvertUTF.o sd_notify.o
	$(CXX) $^ -o $@

.PHONY: clean

clean:
	rm -f tojblockd *.o
