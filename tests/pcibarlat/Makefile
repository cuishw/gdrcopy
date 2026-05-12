DESTBIN ?=
GDRAPI_ARCH ?= $(shell ../../config_arch)

COMMONCFLAGS := -O2
CFLAGS   += $(COMMONCFLAGS)
CXXFLAGS += $(COMMONCFLAGS)
LDLIBS   += -lrt

EXES := gdrcopy_pcibarlat gdrcopy_pcibarbw
LAT_OBJS := pcibarlat.o
BW_OBJS := pcibarbw.o

ifeq ($(GDRAPI_ARCH),X86)
SIMD_OBJS := pcibarlat_memcpy_avx.o pcibarlat_memcpy_sse41.o
LAT_OBJS += $(SIMD_OBJS)
BW_OBJS += $(SIMD_OBJS)
endif

all: $(EXES)

driver:
	$(MAKE) -C driver

gdrcopy_pcibarlat: $(LAT_OBJS)
	$(LINK.cc) -o $@ $^ $(LDLIBS)

gdrcopy_pcibarbw: $(BW_OBJS)
	$(LINK.cc) -o $@ $^ $(LDLIBS)

pcibarlat.o: pcibarlat.cpp
pcibarbw.o: pcibarbw.cpp

pcibarlat_memcpy_avx.o: memcpy_avx.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -mavx -c -o $@ $<

pcibarlat_memcpy_sse41.o: memcpy_sse41.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -msse4.1 -c -o $@ $<

install: all
	@ echo "installing pcibarlat exes in $(DESTBIN)..." && \
	mkdir -p $(DESTBIN) && \
	install -D -v -m u=rwx,g=rx,o=rx $(EXES) -t $(DESTBIN)
	cd $(DESTBIN) && ln -sf gdrcopy_pcibarlat pcibarlat && ln -sf gdrcopy_pcibarbw pcibarbw

clean:
	rm -f *.o $(EXES) *~ core.*
	$(MAKE) -C driver clean

.PHONY: all driver install clean
