all: tojblockd

tojblockd.o: udf.h nbd.h
udf.o: udf.h

tojblockd: tojblockd.o udf.o
