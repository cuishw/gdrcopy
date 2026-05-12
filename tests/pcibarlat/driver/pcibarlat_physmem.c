/*
 * SPDX-License-Identifier: MIT OR GPL-2.0-only
 * Copyright (c) 2026. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <linux/capability.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/types.h>

static unsigned long long phys_addr;
static unsigned long map_size;
static bool wc_mapping = true;

module_param(phys_addr, ullong, 0644);
MODULE_PARM_DESC(phys_addr, "Base physical address to expose through mmap");
module_param(map_size, ulong, 0644);
MODULE_PARM_DESC(map_size, "Number of bytes to expose through mmap");
module_param(wc_mapping, bool, 0644);
MODULE_PARM_DESC(wc_mapping, "Use write-combining page attributes when true");

static int pcibarlat_physmem_open(struct inode *inode, struct file *file)
{
    if (!capable(CAP_SYS_RAWIO))
        return -EPERM;

    if (!phys_addr || !map_size)
        return -EINVAL;

    if (!PAGE_ALIGNED(phys_addr) || !PAGE_ALIGNED(map_size))
        return -EINVAL;

    return 0;
}

static int pcibarlat_physmem_mmap(struct file *file, struct vm_area_struct *vma)
{
    unsigned long requested_size = vma->vm_end - vma->vm_start;
    unsigned long requested_offset = vma->vm_pgoff << PAGE_SHIFT;
    phys_addr_t requested_phys;

    if (!requested_size)
        return -EINVAL;

    if (!PAGE_ALIGNED(requested_size))
        return -EINVAL;

    if (requested_offset > map_size || requested_size > map_size - requested_offset)
        return -EINVAL;

    requested_phys = (phys_addr_t)phys_addr + requested_offset;

    vma->vm_flags |= VM_IO | VM_PFNMAP | VM_DONTEXPAND | VM_DONTDUMP;
    if (wc_mapping)
        vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);
    else
        vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);

    return remap_pfn_range(vma, vma->vm_start, requested_phys >> PAGE_SHIFT,
                           requested_size, vma->vm_page_prot);
}

static const struct file_operations pcibarlat_physmem_fops = {
    .owner = THIS_MODULE,
    .open = pcibarlat_physmem_open,
    .mmap = pcibarlat_physmem_mmap,
};

static struct miscdevice pcibarlat_physmem_device = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = "pcibarlat_physmem",
    .fops = &pcibarlat_physmem_fops,
    .mode = 0600,
};

static int __init pcibarlat_physmem_init(void)
{
    int ret;

    if (!phys_addr || !map_size) {
        pr_err("pcibarlat_physmem: phys_addr and map_size must be set\n");
        return -EINVAL;
    }

    if (!PAGE_ALIGNED(phys_addr) || !PAGE_ALIGNED(map_size)) {
        pr_err("pcibarlat_physmem: phys_addr and map_size must be page aligned\n");
        return -EINVAL;
    }

    ret = misc_register(&pcibarlat_physmem_device);
    if (ret)
        return ret;

    pr_info("pcibarlat_physmem: /dev/%s maps phys 0x%llx size 0x%lx (%s)\n",
            pcibarlat_physmem_device.name, phys_addr, map_size,
            wc_mapping ? "write-combining" : "uncached");
    return 0;
}

static void __exit pcibarlat_physmem_exit(void)
{
    misc_deregister(&pcibarlat_physmem_device);
}

module_init(pcibarlat_physmem_init);
module_exit(pcibarlat_physmem_exit);

MODULE_LICENSE("Dual MIT/GPL");
MODULE_AUTHOR("GDRCopy");
MODULE_DESCRIPTION("Configurable physical memory mmap driver for pcibarlat");
