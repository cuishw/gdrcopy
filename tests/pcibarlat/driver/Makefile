KDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

obj-m := pcibarlat_physmem.o

all:
	@if [ ! -d "$(KDIR)" ]; then \
		echo "ERROR: kernel build directory not found: $(KDIR)"; \
		echo "Set KDIR=<kernel-build-dir> or install kernel headers."; \
		exit 1; \
	fi
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	@if [ -d "$(KDIR)" ]; then \
		$(MAKE) -C $(KDIR) M=$(PWD) clean; \
	else \
		rm -f *.o *.ko *.mod *.mod.c *.mod.o Module.symvers modules.order .*.cmd; \
		rm -rf .tmp_versions; \
	fi

.PHONY: all clean
