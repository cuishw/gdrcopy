DESTBIN ?=
UNAME_M := $(shell uname -m)
ifneq ($(filter x86_64 i386 i486 i586 i686,$(UNAME_M)),)
DETECTED_GDRAPI_ARCH := X86
else ifneq ($(filter aarch64 arm64,$(UNAME_M)),)
DETECTED_GDRAPI_ARCH := ARM64
else ifneq ($(filter ppc64 ppc64le,$(UNAME_M)),)
DETECTED_GDRAPI_ARCH := PPC64
else
DETECTED_GDRAPI_ARCH := UNKNOWN
endif
GDRAPI_ARCH ?= $(DETECTED_GDRAPI_ARCH)

COMMONCFLAGS := -O2
CFLAGS   += $(COMMONCFLAGS)
CXXFLAGS += $(COMMONCFLAGS)
LDLIBS   += -lrt -lpthread

EXES := gdrcopy_pcibarlat gdrcopy_pcibarbw gdrcopy_pcibarstreambw
LAT_OBJS := pcibarlat.o
BW_OBJS := pcibarbw.o
STREAMBW_OBJS := pcibarstreambw.o

ifeq ($(GDRAPI_ARCH),X86)
SIMD_OBJS := pcibarlat_memcpy_avx.o pcibarlat_memcpy_sse41.o
LAT_OBJS += $(SIMD_OBJS)
BW_OBJS += $(SIMD_OBJS)
STREAMBW_OBJS += $(SIMD_OBJS)
endif

all: $(EXES)

driver:
	$(MAKE) -C driver

gdrcopy_pcibarlat: $(LAT_OBJS)
	$(LINK.cc) -o $@ $^ $(LDLIBS)

gdrcopy_pcibarbw: $(BW_OBJS)
	$(LINK.cc) -o $@ $^ $(LDLIBS)

gdrcopy_pcibarstreambw: $(STREAMBW_OBJS)
	$(LINK.cc) -o $@ $^ $(LDLIBS)

pcibarlat.o: pcibarlat.cpp
pcibarbw.o: pcibarbw.cpp
pcibarstreambw.o: pcibarstreambw.cpp

pcibarlat_memcpy_avx.o: memcpy_avx.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -mavx -c -o $@ $<

pcibarlat_memcpy_sse41.o: memcpy_sse41.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -msse4.1 -c -o $@ $<

install: all
	@ echo "installing pcibarlat exes in $(DESTBIN)..." && \
	mkdir -p $(DESTBIN) && \
	install -D -v -m u=rwx,g=rx,o=rx $(EXES) -t $(DESTBIN)
	cd $(DESTBIN) && ln -sf gdrcopy_pcibarlat pcibarlat && ln -sf gdrcopy_pcibarbw pcibarbw && ln -sf gdrcopy_pcibarstreambw pcibarstreambw

clean:
	rm -f *.o $(EXES) *~ core.*
	$(MAKE) -C driver clean

.PHONY: all driver install clean
