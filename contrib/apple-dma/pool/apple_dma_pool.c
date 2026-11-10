// SPDX-License-Identifier: GPL-2.0
/*
 * apple_dma_pool: minimal per-device dma_map_ops that bounce every mapping
 * through a fixed, identity-addressed pool (the DriverKit DMA allocation that
 * QEMU maps at its device DMA address).
 *
 * Experimental and untested against hardware. Every operation is implemented
 * here, so the kernel dispatch patch used by apple_dma is not required, but
 * the kernel must have CONFIG_ARCH_HAS_DMA_OPS and no IOMMU for the device.
 *
 * The pool range must not be used by another driver (for example a pmem
 * namespace) and must not be System RAM; both are the loader's responsibility
 * except for the System RAM check below.
 */
#include <linux/bitops.h>
#include <linux/dma-map-ops.h>
#include <linux/iommu.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/notifier.h>
#include <linux/pci.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/version.h>
#include <linux/vmalloc.h>

#include "apple_dma_pool_core.h"

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 18, 0)
#error "apple_dma_pool requires dma_map_ops.map_phys (Linux 6.18 or newer)"
#endif
#ifndef CONFIG_ARCH_HAS_DMA_OPS
#error "apple_dma_pool requires CONFIG_ARCH_HAS_DMA_OPS"
#endif

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Identity-addressed bounce DMA pool for one PCI device");

static char *bdf;
module_param(bdf, charp, 0444);
MODULE_PARM_DESC(bdf, "PCI device as DDDD:BB:SS.F; it must be unbound");
static unsigned long long pool_base;
module_param(pool_base, ullong, 0444);
MODULE_PARM_DESC(pool_base, "Pool physical address, equal to its DMA address");
static unsigned long long pool_size;
module_param(pool_size, ullong, 0444);
MODULE_PARM_DESC(pool_size, "Pool size in bytes");

static struct {
	struct pci_dev *pdev;
	u64 base;
	u64 size;
	void *virt;
	struct pool_core core;
	spinlock_t lock;
	bool holding_ref;
} pool;

static bool pool_owns(dma_addr_t dma, size_t size)
{
	return dma >= pool.base && size <= pool.size &&
	       dma - pool.base <= pool.size - size;
}

static void *pool_cpu(dma_addr_t dma)
{
	return pool.virt + (dma - pool.base);
}

/* Index of the first page of the range containing page idx; lock held. */
static unsigned long pool_start_of(unsigned long idx)
{
	while (idx && pool.core.slots[idx].kind == POOL_CONT)
		idx--;
	return idx;
}

static dma_addr_t pool_map_one(phys_addr_t phys, size_t size,
			       enum dma_data_direction dir,
			       unsigned long attrs)
{
	size_t offset = phys & (POOL_PAGE_SIZE - 1);
	unsigned long npages, flags;
	long idx;

#ifdef DMA_ATTR_MMIO
	if (attrs & DMA_ATTR_MMIO)
		return DMA_MAPPING_ERROR;
#endif
	if (!size || size > pool.size - offset)
		return DMA_MAPPING_ERROR;
	npages = pool_pages_for(offset, size);

	spin_lock_irqsave(&pool.lock, flags);
	idx = pool_find(&pool.core, npages, 1);
	if (idx >= 0) {
		pool_claim(&pool.core, idx, npages, POOL_STREAM);
		pool.core.slots[idx].dir = dir;
		pool.core.slots[idx].orig_phys = phys;
		pool.core.slots[idx].size = size;
	}
	spin_unlock_irqrestore(&pool.lock, flags);
	if (idx < 0)
		return DMA_MAPPING_ERROR;

	if (!(attrs & DMA_ATTR_SKIP_CPU_SYNC) && dir != DMA_FROM_DEVICE)
		memcpy(pool.virt + ((u64)idx << POOL_PAGE_SHIFT) + offset,
		       phys_to_virt(phys), size);
	return pool.base + ((u64)idx << POOL_PAGE_SHIFT) + offset;
}

/* Copies one slot in the requested direction. */
static void pool_copy(unsigned long idx, size_t rel, size_t len, bool to_dev)
{
	struct pool_slot slot;
	unsigned long flags;
	size_t offset;

	spin_lock_irqsave(&pool.lock, flags);
	slot = pool.core.slots[idx];
	spin_unlock_irqrestore(&pool.lock, flags);

	offset = slot.orig_phys & (POOL_PAGE_SIZE - 1);
	if (slot.kind != POOL_STREAM || rel > slot.size ||
	    len > slot.size - rel) {
		WARN_ONCE(1, "apple_dma_pool: sync outside a streaming mapping\n");
		return;
	}
	if (to_dev)
		memcpy(pool.virt + ((u64)idx << POOL_PAGE_SHIFT) + offset + rel,
		       phys_to_virt(slot.orig_phys) + rel, len);
	else
		memcpy(phys_to_virt(slot.orig_phys) + rel,
		       pool.virt + ((u64)idx << POOL_PAGE_SHIFT) + offset + rel,
		       len);
}

static void pool_unmap_one(dma_addr_t dma, size_t size,
			   enum dma_data_direction dir, unsigned long attrs)
{
	unsigned long idx, flags;
	struct pool_slot slot;

	if (!pool_owns(dma, 1)) {
		WARN_ONCE(1, "apple_dma_pool: unmap of foreign address\n");
		return;
	}
	idx = (dma - pool.base) >> POOL_PAGE_SHIFT;

	spin_lock_irqsave(&pool.lock, flags);
	slot = pool.core.slots[idx];
	spin_unlock_irqrestore(&pool.lock, flags);
	if (slot.kind != POOL_STREAM) {
		WARN_ONCE(1, "apple_dma_pool: unmap of unmapped address\n");
		return;
	}

	if (!(attrs & DMA_ATTR_SKIP_CPU_SYNC) &&
	    (dir == DMA_FROM_DEVICE || dir == DMA_BIDIRECTIONAL))
		pool_copy(idx, 0, slot.size, false);

	spin_lock_irqsave(&pool.lock, flags);
	pool_release(&pool.core, idx, POOL_STREAM);
	spin_unlock_irqrestore(&pool.lock, flags);
}

static dma_addr_t apple_pool_map_phys(struct device *dev, phys_addr_t phys,
				      size_t size, enum dma_data_direction dir,
				      unsigned long attrs)
{
	return pool_map_one(phys, size, dir, attrs);
}

static void apple_pool_unmap_phys(struct device *dev, dma_addr_t dma,
				  size_t size, enum dma_data_direction dir,
				  unsigned long attrs)
{
	pool_unmap_one(dma, size, dir, attrs);
}

static void pool_sync(dma_addr_t dma, size_t size,
		      enum dma_data_direction dir, bool for_cpu)
{
	unsigned long idx, start, flags;
	struct pool_slot slot;
	size_t start_dma;

	if (!pool_owns(dma, size))
		return;
	idx = (dma - pool.base) >> POOL_PAGE_SHIFT;

	spin_lock_irqsave(&pool.lock, flags);
	start = pool_start_of(idx);
	slot = pool.core.slots[start];
	spin_unlock_irqrestore(&pool.lock, flags);
	if (slot.kind != POOL_STREAM)
		return;

	start_dma = ((u64)start << POOL_PAGE_SHIFT) +
		    (slot.orig_phys & (POOL_PAGE_SIZE - 1));
	if (dma - pool.base < start_dma)
		return;
	if (for_cpu && (dir == DMA_FROM_DEVICE || dir == DMA_BIDIRECTIONAL))
		pool_copy(start, dma - pool.base - start_dma, size, false);
	else if (!for_cpu && (dir == DMA_TO_DEVICE || dir == DMA_BIDIRECTIONAL))
		pool_copy(start, dma - pool.base - start_dma, size, true);
}

static void apple_pool_sync_single_for_cpu(struct device *dev, dma_addr_t dma,
					   size_t size,
					   enum dma_data_direction dir)
{
	pool_sync(dma, size, dir, true);
}

static void apple_pool_sync_single_for_device(struct device *dev,
					      dma_addr_t dma, size_t size,
					      enum dma_data_direction dir)
{
	pool_sync(dma, size, dir, false);
}

static void apple_pool_sync_sg_for_cpu(struct device *dev,
				       struct scatterlist *sgl, int nents,
				       enum dma_data_direction dir)
{
	struct scatterlist *sg;
	int i;

	for_each_sg(sgl, sg, nents, i)
		pool_sync(sg_dma_address(sg), sg_dma_len(sg), dir, true);
}

static void apple_pool_sync_sg_for_device(struct device *dev,
					  struct scatterlist *sgl, int nents,
					  enum dma_data_direction dir)
{
	struct scatterlist *sg;
	int i;

	for_each_sg(sgl, sg, nents, i)
		pool_sync(sg_dma_address(sg), sg_dma_len(sg), dir, false);
}

static void apple_pool_unmap_sg(struct device *dev, struct scatterlist *sgl,
				int nents, enum dma_data_direction dir,
				unsigned long attrs)
{
	struct scatterlist *sg;
	int i;

	for_each_sg(sgl, sg, nents, i) {
		if (!sg_dma_len(sg))
			break;
		pool_unmap_one(sg_dma_address(sg), sg_dma_len(sg), dir, attrs);
	}
}

/* Segments are mapped one by one; none are merged. */
static int apple_pool_map_sg(struct device *dev, struct scatterlist *sgl,
			     int nents, enum dma_data_direction dir,
			     unsigned long attrs)
{
	struct scatterlist *sg;
	int i;

	for_each_sg(sgl, sg, nents, i) {
		dma_addr_t dma = pool_map_one(sg_phys(sg), sg->length, dir,
					      attrs);

		if (dma == DMA_MAPPING_ERROR) {
			apple_pool_unmap_sg(dev, sgl, i, dir,
					    attrs | DMA_ATTR_SKIP_CPU_SYNC);
			sg_dma_len(sgl) = 0;
			return -ENOMEM;
		}
		sg_dma_address(sg) = dma;
		sg_dma_len(sg) = sg->length;
	}
	return nents;
}

static void *apple_pool_alloc(struct device *dev, size_t size,
			      dma_addr_t *dma_handle, gfp_t gfp,
			      unsigned long attrs)
{
	unsigned long npages, flags;
	long idx;

	if (!size || size > pool.size)
		return NULL;
	npages = pool_pages_for(0, size);
	if (pool.base + pool.size - 1 > dev->coherent_dma_mask)
		return NULL;

	/* Linux guarantees alignment to the smallest covering page order. */
	spin_lock_irqsave(&pool.lock, flags);
	idx = pool_find(&pool.core, npages, roundup_pow_of_two(npages));
	if (idx >= 0)
		pool_claim(&pool.core, idx, npages, POOL_COHERENT);
	spin_unlock_irqrestore(&pool.lock, flags);
	if (idx < 0)
		return NULL;

	*dma_handle = pool.base + ((u64)idx << POOL_PAGE_SHIFT);
	memset(pool_cpu(*dma_handle), 0, npages << POOL_PAGE_SHIFT);
	return pool_cpu(*dma_handle);
}

static void apple_pool_free(struct device *dev, size_t size, void *vaddr,
			    dma_addr_t dma, unsigned long attrs)
{
	unsigned long flags, freed = 0;

	if (pool_owns(dma, 1) && vaddr == pool_cpu(dma)) {
		spin_lock_irqsave(&pool.lock, flags);
		freed = pool_release(&pool.core,
				     (dma - pool.base) >> POOL_PAGE_SHIFT,
				     POOL_COHERENT);
		spin_unlock_irqrestore(&pool.lock, flags);
	}
	WARN_ONCE(!freed, "apple_dma_pool: free of unknown coherent buffer\n");
}

static int apple_pool_mmap(struct device *dev, struct vm_area_struct *vma,
			   void *cpu_addr, dma_addr_t dma, size_t size,
			   unsigned long attrs)
{
	unsigned long count = vma_pages(vma);
	unsigned long pages = pool_pages_for(0, size);

	if (!pool_owns(dma, size) || vma->vm_pgoff >= pages ||
	    count > pages - vma->vm_pgoff)
		return -ENXIO;
	return remap_pfn_range(vma, vma->vm_start,
			       PHYS_PFN(dma) + vma->vm_pgoff,
			       count << PAGE_SHIFT, vma->vm_page_prot);
}

static int apple_pool_get_sgtable(struct device *dev, struct sg_table *sgt,
				  void *cpu_addr, dma_addr_t dma, size_t size,
				  unsigned long attrs)
{
	/* Pool memory without struct page cannot be described to callers. */
	if (!pool_owns(dma, size) || !pfn_valid(PHYS_PFN(dma)))
		return -ENXIO;
	if (sg_alloc_table(sgt, 1, GFP_KERNEL))
		return -ENOMEM;
	sg_set_page(sgt->sgl, pfn_to_page(PHYS_PFN(dma)),
		    PAGE_ALIGN(size), 0);
	return 0;
}

static int apple_pool_dma_supported(struct device *dev, u64 mask)
{
	return mask >= pool.base + pool.size - 1;
}

static u64 apple_pool_get_required_mask(struct device *dev)
{
	return DMA_BIT_MASK(fls64(pool.base + pool.size - 1));
}

static size_t apple_pool_max_mapping_size(struct device *dev)
{
	return pool.size / 8;
}

static const struct dma_map_ops apple_pool_ops = {
	.alloc = apple_pool_alloc,
	.free = apple_pool_free,
	.mmap = apple_pool_mmap,
	.get_sgtable = apple_pool_get_sgtable,
	.map_phys = apple_pool_map_phys,
	.unmap_phys = apple_pool_unmap_phys,
	.map_sg = apple_pool_map_sg,
	.unmap_sg = apple_pool_unmap_sg,
	.sync_single_for_cpu = apple_pool_sync_single_for_cpu,
	.sync_single_for_device = apple_pool_sync_single_for_device,
	.sync_sg_for_cpu = apple_pool_sync_sg_for_cpu,
	.sync_sg_for_device = apple_pool_sync_sg_for_device,
	.dma_supported = apple_pool_dma_supported,
	.get_required_mask = apple_pool_get_required_mask,
	.max_mapping_size = apple_pool_max_mapping_size,
};

/* While a driver is bound the ops are live, so pin the module. */
static int apple_pool_bus_event(struct notifier_block *nb,
				unsigned long action, void *data)
{
	struct device *dev = data;

	if (!pool.pdev || dev != &pool.pdev->dev)
		return NOTIFY_DONE;
	if (action == BUS_NOTIFY_BOUND_DRIVER && !pool.holding_ref) {
		pool.holding_ref = try_module_get(THIS_MODULE);
	} else if (action == BUS_NOTIFY_UNBOUND_DRIVER && pool.holding_ref) {
		pool.holding_ref = false;
		module_put(THIS_MODULE);
	}
	return NOTIFY_OK;
}

static struct notifier_block apple_pool_nb = {
	.notifier_call = apple_pool_bus_event,
};

static int __init apple_pool_init(void)
{
	unsigned int domain, bus, slot, func;
	struct pci_dev *pdev;
	int ret;

	if (!bdf || sscanf(bdf, "%x:%x:%x.%x", &domain, &bus, &slot,
			   &func) != 4 || slot > 31 || func > 7 || bus > 255) {
		pr_err("apple_dma_pool: bdf=DDDD:BB:SS.F is required\n");
		return -EINVAL;
	}
	if (!pool_size || (pool_base | pool_size) & (POOL_PAGE_SIZE - 1) ||
	    pool_base + pool_size < pool_base) {
		pr_err("apple_dma_pool: invalid pool_base/pool_size\n");
		return -EINVAL;
	}
	if (region_intersects(pool_base, pool_size, IORESOURCE_SYSTEM_RAM,
			      IORES_DESC_NONE) != REGION_DISJOINT) {
		pr_err("apple_dma_pool: pool overlaps System RAM; refusing\n");
		return -EBUSY;
	}

	pdev = pci_get_domain_bus_and_slot(domain, bus, PCI_DEVFN(slot, func));
	if (!pdev)
		return -ENODEV;

	pool.base = pool_base;
	pool.size = pool_size;
	pool.core.npages = pool_size >> POOL_PAGE_SHIFT;
	spin_lock_init(&pool.lock);
	pool.core.slots = vcalloc(pool.core.npages, sizeof(*pool.core.slots));
	pool.virt = memremap(pool_base, pool_size, MEMREMAP_WB);
	if (!pool.core.slots || !pool.virt) {
		ret = -ENOMEM;
		goto err_free;
	}

	ret = bus_register_notifier(&pci_bus_type, &apple_pool_nb);
	if (ret)
		goto err_free;

	device_lock(&pdev->dev);
	if (pdev->dev.driver || device_iommu_mapped(&pdev->dev) ||
	    pdev->dev.dma_ops) {
		ret = -EBUSY;
	} else {
		pool.pdev = pdev;
#ifdef CONFIG_DMA_OPS_BYPASS
		pdev->dev.dma_ops_bypass = false;
#endif
		pdev->dev.dma_ops = &apple_pool_ops;
		ret = 0;
	}
	device_unlock(&pdev->dev);
	if (ret) {
		pr_err("apple_dma_pool: %s is bound, IOMMU-managed or already has DMA ops\n",
		       bdf);
		goto err_notifier;
	}

	pr_info("apple_dma_pool: %s uses pool %#llx+%#llx\n", bdf, pool_base,
		pool_size);
	return 0;

err_notifier:
	bus_unregister_notifier(&pci_bus_type, &apple_pool_nb);
err_free:
	if (pool.virt)
		memunmap(pool.virt);
	vfree(pool.core.slots);
	pci_dev_put(pdev);
	memset(&pool, 0, sizeof(pool));
	return ret;
}

static void __exit apple_pool_exit(void)
{
	unsigned long i, used = 0;

	/* Unloading is impossible while a driver is bound, see the notifier. */
	pool.pdev->dev.dma_ops = NULL;
	bus_unregister_notifier(&pci_bus_type, &apple_pool_nb);
	for (i = 0; i < pool.core.npages; i++)
		used += pool.core.slots[i].kind != POOL_FREE;
	WARN(used, "apple_dma_pool: unloading with %lu live pages\n", used);
	memunmap(pool.virt);
	vfree(pool.core.slots);
	pci_dev_put(pool.pdev);
}

module_init(apple_pool_init);
module_exit(apple_pool_exit);
