# pcibarlat physical-memory mmap driver

`pcibarlat_physmem` is a small helper driver for `gdrcopy_pcibarlat`.  It
creates `/dev/pcibarlat_physmem` and maps a configurable physical address range
with write-combining attributes, similar to using a PCI sysfs `resource<N>_wc`
file.

Build the benchmark and driver independently:

```shell
make -C tests/pcibarlat
make -C tests/pcibarlat driver
```

Load the driver with a page-aligned physical base address and size:

```shell
sudo insmod tests/pcibarlat/driver/pcibarlat_physmem.ko \
    phys_addr=0x8800000000 map_size=0x800000 wc_mapping=1
```

Run `gdrcopy_pcibarlat` against the character device:

```shell
sudo tests/pcibarlat/gdrcopy_pcibarlat -f /dev/pcibarlat_physmem -s 8M -R
```

Use the benchmark `-o <offset>` option to select an offset inside the configured
range.  The requested offset plus size must fit within `map_size`.

> **Warning:** mapping arbitrary physical memory is dangerous.  Only expose a
> safe PCI BAR or device memory window and unload the module when finished.
