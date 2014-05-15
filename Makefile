all: tojblockd

DBG=-g
CXXFLAGS=-W -Wall -O2 $(DBG) -Iimport
CFLAGS=-W -Wall -O2 $(DBG) -Iimport

tojblockd.o: vfat.h import/nbd.h import/sd_notify.h
vfat.o: vfat.h import/ConvertUTF.h fat.h dir.h filemap.h
fat.o: fat.h dir.h filemap.h
dir.o: dir.h vfat.h fat.h
filemap.o: filemap.h

import/ConvertUTF.o: import/ConvertUTF.h
import/sd_notify.o: import/sd_notify.h

tojblockd: tojblockd.o vfat.o import/ConvertUTF.o import/sd_notify.o fat.o dir.o filemap.o
	$(CXX) $^ -o $@

.PHONY: clean tests check coverage

clean:
	rm -f tojblockd *.o import/*.o
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

coverage: tests
	lcov --zerocounters -d tests
	-$(MAKE) check
	geninfo tests  # creates the .info tracefiles
	lcov -e tests/fat/fat.*.info $$PWD/fat.cpp -o tests/fat.info
	lcov -e tests/dir/dir.*.info $$PWD/dir.cpp -o tests/dir.info

covhtml: coverage
	genhtml -o covhtml --demangle-cpp tests/*.info
