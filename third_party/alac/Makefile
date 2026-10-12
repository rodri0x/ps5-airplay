CC ?= cc
CXX ?= c++
AR ?= ar
CFLAGS ?= -O2
CXXFLAGS ?= -O2

OBJS = EndianPortable.o ALACBitUtilities.o ALACDecoder.o ag_dec.o dp_dec.o matrix_dec.o

libalac.a: $(OBJS)
	$(AR) crD $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -Icodec -c -o $@ $<

ALACDecoder.o: codec/ALACDecoder.cpp
	$(CXX) $(CXXFLAGS) -Icodec -c -o $@ $<

ALACBitUtilities.o: codec/ALACBitUtilities.c
	$(CC) $(CFLAGS) -Icodec -c -o $@ $<

EndianPortable.o: codec/EndianPortable.c
	$(CC) $(CFLAGS) -Icodec -c -o $@ $<

ag_dec.o: codec/ag_dec.c
	$(CC) $(CFLAGS) -Icodec -c -o $@ $<

dp_dec.o: codec/dp_dec.c
	$(CC) $(CFLAGS) -Icodec -c -o $@ $<

matrix_dec.o: codec/matrix_dec.c
	$(CC) $(CFLAGS) -Icodec -c -o $@ $<

.PHONY: clean
clean:
	find . -maxdepth 1 -name '*.o' -delete
	find . -maxdepth 1 -name '*.a' -delete
