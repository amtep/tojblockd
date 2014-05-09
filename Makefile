all: tojblockd

DBG=-g
CXXFLAGS=-W -Wall -O2 $(DBG)
CFLAGS=-W -Wall -O2 $(DBG)

tojblockd.o: vfat.h nbd.h fat.h
vfat.o: vfat.h ConvertUTF.h
ConvertUTF.o: ConvertUTF.h
fat.o: fat.h
dir.o: dir.h

tojblockd: tojblockd.o vfat.o ConvertUTF.o sd_notify.o fat.o dir.o
	$(CXX) $^ -o $@

.PHONY: clean tests check coverage

clean:
	rm -f tojblockd *.o
	if [ -e tests/Makefile ]; then cd tests && $(MAKE) distclean; fi
	rm -f tests/*.info tests/*/*.gcda tests/*/*.gcno tests/*/*.info
	rm -rf covhtml

tests: tests/Makefile
	cd tests && $(MAKE)

tests/Makefile:
	cd tests && qmake

check: tests
	tests/fat/test-fat
	tests/dir/test-dir

coverage: check
	geninfo tests  # creates the .info tracefiles
	lcov -e tests/fat/fat.*.info $$PWD/fat.cpp -o tests/fat.info
	lcov -e tests/dir/dir.*.info $$PWD/dir.cpp -o tests/dir.info

covhtml: coverage
	genhtml -o covhtml --demangle-cpp tests/*.info
