DESTBIN ?=
GDRAPI_ARCH ?= $(shell ../../config_arch)

COMMONCFLAGS := -O2
CFLAGS   += $(COMMONCFLAGS)
CXXFLAGS += $(COMMONCFLAGS)
LDLIBS   += -lrt

EXE := gdrcopy_pcibarlat
OBJS := pcibarlat.o

ifeq ($(GDRAPI_ARCH),X86)
SIMD_OBJS := pcibarlat_memcpy_avx.o pcibarlat_memcpy_sse41.o
OBJS += $(SIMD_OBJS)
endif

all: $(EXE)

$(EXE): $(OBJS)
	$(LINK.cc) -o $@ $^ $(LDLIBS)

pcibarlat.o: pcibarlat.cpp

pcibarlat_memcpy_avx.o: memcpy_avx.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -mavx -c -o $@ $<

pcibarlat_memcpy_sse41.o: memcpy_sse41.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -msse4.1 -c -o $@ $<

install: all
	@ echo "installing $(EXE) in $(DESTBIN)..." && \
	mkdir -p $(DESTBIN) && \
	install -D -v -m u=rwx,g=rx,o=rx $(EXE) -t $(DESTBIN)
	cd $(DESTBIN) && ln -sf $(EXE) pcibarlat

clean:
	rm -f *.o $(EXE) *~ core.*

.PHONY: all install clean
