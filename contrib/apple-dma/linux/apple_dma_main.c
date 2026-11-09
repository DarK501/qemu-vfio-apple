// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * apple_dma - guest kernel module for the apple-dma-pci paravirtual device.
 *
 * Intercepts DMA map/unmap operations on the managed passthrough PCI
 * device and forwards them to the QEMU
 * apple-dma-pci device. This lets the host DriverKit extension establish
 * device-visible DMA mappings for PCI passthrough.
 *
 * Transport protocol (must match hw/vfio/apple-dma.c):
 *   1. Driver maps BAR0, reads VERSION / MANAGED_BDF / MAX_ENTRIES.
 *   2. Allocates a 32-byte command page, writes its GPA to the BAR.
 *   3. Per batch: fills command page + request buffer in RAM (no VMEXIT),
 *      then writes the doorbell register (single VMEXIT).
 *   4. When the doorbell write returns, responses + status are in guest RAM.
 */

#include <linux/device.h>
#include <linux/dma-map-ops.h>
#include <linux/err.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

#include "apple-dma-protocol.h"
#include <linux/version.h>

#include <linux/hash.h>

#include "apple_dma_trace.h"
#ifdef CONFIG_ARM64
#include "quirks/dczid_patch.h"
#include "quirks/tso_patch.h"
#include "quirks/uvm_page_patch.h"
#endif

/*
 * This module hooks per-device dma_map_ops, which requires the dma_ops field
 * in struct device. Older kernels guard that field with CONFIG_DMA_OPS;
 * newer kernels use CONFIG_ARCH_HAS_DMA_OPS.
 */
#if !defined(CONFIG_ARCH_HAS_DMA_OPS) && !defined(CONFIG_DMA_OPS)
#error "apple_dma requires per-device DMA operations in struct device"
#endif

#if defined(CONFIG_X86) && !defined(DMA_MAP_OPS_EXPLICIT_DISPATCH)
#error "apple_dma on x86 requires the explicit DMA ops dispatch kernel patch"
#endif

/*
 * Linux 6.10 renamed .alloc_pages to .alloc_pages_op in struct dma_map_ops
 * to avoid a conflict with the alloc_pages() macro added for memory-allocation
 * profiling.
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 10, 0)
#define APPLE_DMA_ALLOC_PAGES_FIELD alloc_pages_op
#else
#define APPLE_DMA_ALLOC_PAGES_FIELD alloc_pages
#endif

/*
 * Linux 6.18+ removed .map_page/.unmap_page from struct dma_map_ops in favour
 * of .map_phys/.unmap_phys (phys_addr_t instead of struct page * + offset).
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 18, 0)
#define APPLE_DMA_HAS_MAP_PHYS
#endif

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Apple DMA PCI guest module");
MODULE_AUTHOR("QEMU apple-dma");

/* --- PCI IDs (must match hw/vfio/apple-dma.c) --- */
#define APPLE_DMA_PCI_VENDOR	0x1B36	/* PCI_VENDOR_ID_REDHAT */
#define APPLE_DMA_PCI_DEVICE	0x0015	/* PCI_DEVICE_ID_REDHAT_APPLE_DMA */

/* --- BAR0 register offsets --- */
#define REG_VERSION		0x00
#define REG_MANAGED_BDF		0x04
#define REG_MAX_ENTRIES		0x08
#define REG_STATUS		0x0C
#define REG_CMD_GPA_LO		0x10
#define REG_CMD_GPA_HI		0x14
#define REG_DOORBELL		0x18

#define APPLE_DMA_VERSION	APPLE_DMA_PROTOCOL_VERSION

/* Command types */
#define CMD_MAP			1
#define CMD_UNMAP		2

/* Status codes */
#define S_OK			0
#define S_IOERR			1
#define S_INVAL			3

/* --- Wire structures (must match QEMU device layout exactly) --- */

struct apple_dma_cmd_page {
	__le32 type;
	__le32 count;
	__le32 status;
	__le32 reserved;
	__le64 req_gpa;
	__le64 resp_gpa;
} __packed;

struct apple_dma_map_req {
	__le64 gpa;
	__le32 len;
	__le32 flags;
} __packed;

struct apple_dma_unmap_req {
	__le64 iova;
	__le64 size;
} __packed;

struct apple_dma_unmap_resp {
	__le64 iova;
	__le32 status;
	__le32 reserved;
} __packed;

/* --- Module parameters --- */

static unsigned short managed_vendor = 0xffff;
static unsigned short managed_device = 0xffff;
#ifdef CONFIG_ARM64
static bool enable_quirks = true;
#else
static bool enable_quirks;
#endif

module_param(managed_vendor, ushort, 0444);
MODULE_PARM_DESC(managed_vendor,
		 "Optional PCI vendor id filter for the managed passthrough device");
module_param(managed_device, ushort, 0444);
MODULE_PARM_DESC(managed_device,
		 "Optional PCI device id filter for the managed passthrough device");
module_param(enable_quirks, bool, 0444);
MODULE_PARM_DESC(enable_quirks,
		 "Enable vendor-specific workaround quirks (dczid and uvm page-size)");
#ifdef CONFIG_ARM64
static unsigned int window_shift = 18;	/* 256K default for arm64 */
#else
static unsigned int window_shift;	/* Exact mappings by default */
#endif
module_param(window_shift, uint, 0444);
MODULE_PARM_DESC(window_shift,
		 "Log2 of DMA window coalescing size (0=off; x86 defaults to off)");
#ifdef CONFIG_ARM64
static bool enable_tso = true;
#else
static bool enable_tso;
#endif
module_param(enable_tso, bool, 0444);
MODULE_PARM_DESC(enable_tso,
		 "Enable Apple TSO mode on every online CPU at module load "
		 "and install a PR_{GET,SET}_MEM_MODEL prctl shim.  Only "
		 "meaningful when running under HVF on an Apple silicon host.");

/* --- Internal data structures --- */

/*
 * A dma_window is a single bus-address reservation shared by one or more
 * guest-level DMA mappings.  Two shapes live in the same struct:
 *
 *   - Windowed: base_gpa is wnd_mask-aligned and size == wnd_size.  Many
 *     small guest mappings that fall inside the window all share this one
 *     host mapping. This is the common case and is what gives us the huge
 *     entry-count reduction.
 *
 *   - Direct: base_gpa and size are exactly what the guest asked for.
 *     Used when a mapping is too large to fit in a single window, or when
 *     windowing is disabled.  Still refcounted, so when a guest driver
 *     legitimately maps the same (gpa, size) twice (e.g. two PRIME dma-buf
 *     imports of the same virtio-gpu framebuffer) we hand back the same
 *     bus address and only call the host on the first and last reference.
 *
 * In either case the host sees exactly one register/unregister per window.
 */
struct dma_window {
	struct hlist_node hnode;
	phys_addr_t base_gpa;
	size_t      size;
	dma_addr_t  dma_addr;
	u64         iova;
	unsigned int refcount;
};

struct apple_dma_map_entry {
	struct list_head node;
	dma_addr_t mapped_dma;	/* DMA address returned to the managed device driver */
	dma_addr_t orig_dma;	/* DMA address from original map path */
	size_t size;
	struct dma_window *window; /* owning bus-address reservation */
};

struct apple_dma_sg_mapping {
	struct list_head node;
	struct scatterlist *sg;
	int nents;
	int orig_mapped;
	dma_addr_t *mapped_dma_addrs;
	dma_addr_t *orig_dma_addrs;
	unsigned int *orig_dma_lens;
	size_t *sizes;
};

#define DMA_WINDOW_HT_BITS	12
#define DMA_WINDOW_HT_SIZE	(1 << DMA_WINDOW_HT_BITS)

struct apple_dma_dev {
	struct list_head instance_node;	/* linkage in apple_dma_instances */
	struct pci_dev *pdev;
	void __iomem *bar;
	struct apple_dma_cmd_page *cmd_page;
	struct mutex batch_lock;
	spinlock_t maps_lock;
	struct list_head maps;
	struct list_head sg_maps;
	struct pci_dev *managed_pdev;
	const struct dma_map_ops *orig_dma_ops;
	u16 managed_bdf;
	u32 max_entries;
	bool ready;
	atomic_t map_count;	/* number of active mappings */
	atomic64_t map_bytes;	/* total bytes currently mapped */

	/* Window coalescing (protected by batch_lock) */
	struct hlist_head window_ht[DMA_WINDOW_HT_SIZE];
	unsigned int window_count;

	/*
	 * Pre-allocated req/resp buffers for device commands.
	 * Must live in kzalloc'd memory (linear map) because virt_to_phys()
	 * doesn't work on vmalloc'd kernel stacks (VMAP_STACK).
	 * Protected by batch_lock.
	 */
	struct apple_dma_map_req map_req;
	AppleDMAMapResult map_resp;
	struct apple_dma_unmap_req unmap_req;
	struct apple_dma_unmap_resp unmap_resp;

#ifdef CONFIG_ARM64
	bool dczid_patched;
	bool uvm_page_patched;
#endif
};

static LIST_HEAD(apple_dma_instances);
static DEFINE_MUTEX(instances_lock);

#ifdef CONFIG_ARM64
/* Global quirk reference counts — quirks are applied once system-wide */
static int dczid_refcount;
static int uvm_page_refcount;
#endif

/* ------------------------------------------------------------------ */
/* Window coalescing globals (computed from window_shift at init)       */
/* ------------------------------------------------------------------ */

static u64 wnd_size;	/* 1 << window_shift, or 0 if disabled */
static u64 wnd_mask;	/* ~(wnd_size - 1) */

static inline bool wnd_enabled(void)
{
	return wnd_size != 0;
}

/*
 * Hash on page-granular gpa so both window-aligned entries (windowed case)
 * and arbitrary gpas (direct case) distribute across buckets evenly.
 */
static inline u32 wnd_hash(phys_addr_t base_gpa)
{
	return hash_long((unsigned long)(base_gpa >> PAGE_SHIFT),
			 DMA_WINDOW_HT_BITS);
}

/*
 * Look up the apple_dma_dev instance responsible for a given managed device.
 * Called from DMA ops callbacks where @dev is the managed passthrough device.
 */
static struct apple_dma_dev *apple_dma_find_by_dev(struct device *dev)
{
	struct apple_dma_dev *ad;

	list_for_each_entry(ad, &apple_dma_instances, instance_node) {
		if (&ad->managed_pdev->dev == dev)
			return ad;
	}
	return NULL;
}

struct apple_dma_dev *apple_dma_get(void)
{
	if (list_empty(&apple_dma_instances))
		return NULL;
	return list_first_entry(&apple_dma_instances,
				struct apple_dma_dev, instance_node);
}

void apple_dma_trace_get_counts(struct apple_dma_dev *ad, u32 *maps,
				u64 *bytes, u32 *window_count)
{
	if (maps)
		*maps = ad ? (u32)atomic_read(&ad->map_count) : 0;
	if (bytes)
		*bytes = ad ? (u64)atomic64_read(&ad->map_bytes) : 0;
	if (window_count)
		*window_count = ad ? ad->window_count : 0;
}

/*
 * Aggregate counts across all instances.  Used by the trace snapshot.
 */
void apple_dma_trace_get_total_counts(u32 *maps, u64 *bytes,
				      u32 *window_count)
{
	struct apple_dma_dev *ad;
	u32 m = 0;
	u64 b = 0;
	u32 w = 0;

	list_for_each_entry(ad, &apple_dma_instances, instance_node) {
		m += (u32)atomic_read(&ad->map_count);
		b += (u64)atomic64_read(&ad->map_bytes);
		w += ad->window_count;
	}
	if (maps)
		*maps = m;
	if (bytes)
		*bytes = b;
	if (window_count)
		*window_count = w;
}

void apple_dma_trace_get_window_config(unsigned int *shift, u64 *size)
{
	if (shift)
		*shift = window_shift;
	if (size)
		*size = wnd_size;
}

static bool apple_dma_usable(struct apple_dma_dev *ad)
{
	if (!ad || !ad->ready)
		return false;
	/* Batch submission holds a mutex — cannot be in atomic context. */
	if (in_atomic() || irqs_disabled())
		return false;
	return true;
}

static __noreturn void apple_dma_panic_state(struct device *dev,
					     const char *reason)
{
	panic("apple_dma: fatal: %s on %s\n", reason, dev_name(dev));
}

#ifndef CONFIG_X86
#define APPLE_DMA_CALL_ORIG_RET(ad, dev, expr)				\
({									\
	struct device *__dev = (dev);					\
	const struct dma_map_ops *saved__ = get_dma_ops(__dev);		\
	typeof(expr) ret__;						\
									\
	set_dma_ops(__dev, (ad) ? (ad)->orig_dma_ops : NULL);		\
	ret__ = (expr);							\
	set_dma_ops(__dev, saved__);					\
	ret__;								\
})

#define APPLE_DMA_CALL_ORIG_VOID(ad, dev, expr)				\
do {									\
	struct device *__dev = (dev);					\
	const struct dma_map_ops *saved__ = get_dma_ops(__dev);		\
									\
	set_dma_ops(__dev, (ad) ? (ad)->orig_dma_ops : NULL);		\
	expr;								\
	set_dma_ops(__dev, saved__);					\
} while (0)
#endif

#ifdef APPLE_DMA_HAS_MAP_PHYS
static dma_addr_t apple_dma_orig_map_phys(struct apple_dma_dev *ad,
					  struct device *dev, phys_addr_t phys,
					  size_t size,
					  enum dma_data_direction dir,
					  unsigned long attrs)
{
#ifdef CONFIG_X86
	return dma_map_phys_with_ops(dev, ad ? ad->orig_dma_ops : NULL,
				     phys, size, dir, attrs);
#else
	return APPLE_DMA_CALL_ORIG_RET(ad, dev,
				       dma_map_phys(dev, phys, size, dir, attrs));
#endif
}

static void apple_dma_orig_unmap_phys(struct apple_dma_dev *ad,
				      struct device *dev, dma_addr_t addr,
				      size_t size,
				      enum dma_data_direction dir,
				      unsigned long attrs)
{
#ifdef CONFIG_X86
	dma_unmap_phys_with_ops(dev, ad ? ad->orig_dma_ops : NULL, addr, size,
				dir, attrs);
#else
	APPLE_DMA_CALL_ORIG_VOID(ad, dev,
				 dma_unmap_phys(dev, addr, size, dir, attrs));
#endif
}
#else
static dma_addr_t apple_dma_orig_map_page(struct apple_dma_dev *ad,
					  struct device *dev, struct page *page,
					  unsigned long offset, size_t size,
					  enum dma_data_direction dir,
					  unsigned long attrs)
{
#ifdef CONFIG_X86
	return dma_map_page_attrs_with_ops(dev, ad ? ad->orig_dma_ops : NULL,
					   page, offset, size, dir, attrs);
#else
	return APPLE_DMA_CALL_ORIG_RET(ad, dev,
				       dma_map_page_attrs(dev, page, offset, size,
							  dir, attrs));
#endif
}

static void apple_dma_orig_unmap_page(struct apple_dma_dev *ad,
				      struct device *dev, dma_addr_t addr,
				      size_t size,
				      enum dma_data_direction dir,
				      unsigned long attrs)
{
#ifdef CONFIG_X86
	dma_unmap_page_attrs_with_ops(dev, ad ? ad->orig_dma_ops : NULL, addr,
				      size, dir, attrs);
#else
	APPLE_DMA_CALL_ORIG_VOID(ad, dev,
				 dma_unmap_page_attrs(dev, addr, size, dir, attrs));
#endif
}
#endif

static void apple_dma_orig_unmap_sg(struct apple_dma_dev *ad,
				    struct device *dev, struct scatterlist *sg,
				    int nents, enum dma_data_direction dir,
				    unsigned long attrs)
{
#ifdef CONFIG_X86
	dma_unmap_sg_attrs_with_ops(dev, ad ? ad->orig_dma_ops : NULL, sg,
				    nents, dir, attrs);
#else
	APPLE_DMA_CALL_ORIG_VOID(ad, dev,
				 dma_unmap_sg_attrs(dev, sg, nents, dir, attrs));
#endif
}

static void *apple_dma_orig_alloc_attrs(struct apple_dma_dev *ad,
					struct device *dev, size_t size,
					dma_addr_t *dma_handle, gfp_t gfp,
					unsigned long attrs)
{
#ifdef CONFIG_X86
	return dma_alloc_attrs_with_ops(dev, ad ? ad->orig_dma_ops : NULL,
					size, dma_handle, gfp, attrs);
#else
	return APPLE_DMA_CALL_ORIG_RET(ad, dev,
				       dma_alloc_attrs(dev, size, dma_handle,
						       gfp, attrs));
#endif
}

static void apple_dma_orig_free_attrs(struct apple_dma_dev *ad,
				      struct device *dev, size_t size,
				      void *cpu_addr, dma_addr_t dma_handle,
				      unsigned long attrs)
{
#ifdef CONFIG_X86
	dma_free_attrs_with_ops(dev, ad ? ad->orig_dma_ops : NULL, size,
				cpu_addr, dma_handle, attrs);
#else
	APPLE_DMA_CALL_ORIG_VOID(ad, dev,
				 dma_free_attrs(dev, size, cpu_addr, dma_handle,
						attrs));
#endif
}

static struct page *apple_dma_orig_alloc_pages(struct apple_dma_dev *ad,
					       struct device *dev, size_t size,
					       dma_addr_t *dma_handle,
					       enum dma_data_direction dir,
					       gfp_t gfp)
{
#ifdef CONFIG_X86
	return dma_alloc_pages_with_ops(dev, ad ? ad->orig_dma_ops : NULL,
					 size, dma_handle, dir, gfp);
#else
	return APPLE_DMA_CALL_ORIG_RET(ad, dev,
				       dma_alloc_pages(dev, size, dma_handle,
						       dir, gfp));
#endif
}

static void apple_dma_orig_free_pages(struct apple_dma_dev *ad,
				      struct device *dev, size_t size,
				      struct page *page,
				      dma_addr_t dma_handle,
				      enum dma_data_direction dir)
{
#ifdef CONFIG_X86
	dma_free_pages_with_ops(dev, ad ? ad->orig_dma_ops : NULL, size, page,
				dma_handle, dir);
#else
	APPLE_DMA_CALL_ORIG_VOID(ad, dev,
				 dma_free_pages(dev, size, page, dma_handle, dir));
#endif
}

/* ------------------------------------------------------------------ */
/* Batch submission via shared command page + doorbell                  */
/* ------------------------------------------------------------------ */

/*
 * Ring the doorbell.  Caller must hold ad->batch_lock and have filled the
 * command page.  When this returns, the device has processed the batch and
 * written responses + status back to guest RAM.
 */
static u32 apple_dma_ring(struct apple_dma_dev *ad)
{
	/* Ensure cmd page + request buffer writes are visible */
	wmb();
	iowrite32(1, ad->bar + REG_DOORBELL);
	/* Ensure we see response writes from the device */
	rmb();
	return ioread32(ad->bar + REG_STATUS);
}

static void apple_dma_prepare_cmd(struct apple_dma_dev *ad, u32 type,
				  void *req, void *resp)
{
	ad->cmd_page->type     = cpu_to_le32(type);
	ad->cmd_page->count    = cpu_to_le32(1);
	ad->cmd_page->status   = 0;
	ad->cmd_page->reserved = 0;
	ad->cmd_page->req_gpa  = cpu_to_le64(virt_to_phys(req));
	ad->cmd_page->resp_gpa = cpu_to_le64(virt_to_phys(resp));
}

static int apple_dma_device_unmap(struct apple_dma_dev *ad,
				  u64 iova, u64 size);

static int apple_dma_reject_mapping(struct apple_dma_dev *ad, u64 iova,
				    u64 size, int error)
{
	if (apple_dma_device_unmap(ad, iova, size))
		apple_dma_panic_state(&ad->pdev->dev,
				      "invalid map result could not be unmapped");
	return error;
}

/*
 * Map a single region through the device.  Caller must hold ad->batch_lock.
 * On success, fills *out_iova, *out_dma, *out_len and returns 0.
 */
static int apple_dma_device_map(struct apple_dma_dev *ad,
				phys_addr_t gpa, u32 size,
				u64 *out_iova, dma_addr_t *out_dma,
				u32 *out_len)
{
	struct apple_dma_map_req *req = &ad->map_req;
	AppleDMAMapResult *resp = &ad->map_resp;
	dma_addr_t first_dma = 0;
	u64 covered = 0;
	u32 nsegments;
	u32 i;
	u32 status;

	req->gpa = cpu_to_le64(gpa);
	req->len = cpu_to_le32(size);
	req->flags = 0;
	memset(resp, 0, sizeof(*resp));

	apple_dma_prepare_cmd(ad, CMD_MAP, req, resp);
	status = apple_dma_ring(ad);

	if (status == S_INVAL)
		return -EINVAL;

	if (le32_to_cpu(resp->status) == APPLE_DMA_MAP_STATUS_RETAINED) {
		if (le64_to_cpu(resp->mapping_id) != gpa)
			apple_dma_panic_state(&ad->pdev->dev,
					      "retained mapping has invalid token");
		return apple_dma_reject_mapping(ad, gpa, size, -EIO);
	}
	if (le32_to_cpu(resp->status) == APPLE_DMA_MAP_STATUS_ERROR)
		return -EIO;
	if (le32_to_cpu(resp->status) != APPLE_DMA_MAP_STATUS_OK)
		apple_dma_panic_state(&ad->pdev->dev,
				      "unknown DMA map status; mapping may be live");
	if (status != S_OK)
		return apple_dma_reject_mapping(ad, gpa, size, -EIO);

	nsegments = le32_to_cpu(resp->segment_count);
	if (!nsegments || nsegments > APPLE_DMA_MAX_SEGMENTS)
		return apple_dma_reject_mapping(ad, gpa, size, -EIO);

	*out_iova = le64_to_cpu(resp->mapping_id);
	if (*out_iova != gpa)
		return apple_dma_reject_mapping(ad, gpa, size, -EIO);

	for (i = 0; i < nsegments; i++) {
		u64 address = le64_to_cpu(resp->segments[i].address);
		u64 length = le64_to_cpu(resp->segments[i].length);
		u64 offset = le64_to_cpu(resp->segments[i].offset);

		if (!length || offset != covered || covered > size ||
		    length > size - covered ||
		    address > (u64)(dma_addr_t)~0 ||
		    length - 1 > (u64)(dma_addr_t)~0 - address)
			return apple_dma_reject_mapping(ad, *out_iova, size, -EIO);
		if (!i)
			first_dma = (dma_addr_t)address;
		else if (covered > (u64)~0ULL - first_dma ||
			 address != first_dma + covered)
			return apple_dma_reject_mapping(ad, *out_iova, size, -ERANGE);
		covered += length;
	}

	if (covered != size)
		return apple_dma_reject_mapping(ad, *out_iova, size, -EIO);

	*out_dma = first_dma;
	*out_len = size;
	return 0;
}

/*
 * Unmap a single region from the device.  Caller must hold ad->batch_lock.
 */
static int apple_dma_device_unmap(struct apple_dma_dev *ad,
				  u64 iova, u64 size)
{
	struct apple_dma_unmap_req *req = &ad->unmap_req;
	struct apple_dma_unmap_resp *resp = &ad->unmap_resp;
	u32 status;

	req->iova = cpu_to_le64(iova);
	req->size = cpu_to_le64(size);
	memset(resp, 0, sizeof(*resp));

	apple_dma_prepare_cmd(ad, CMD_UNMAP, req, resp);
	status = apple_dma_ring(ad);

	if (status != S_OK)
		return -EIO;

	return le32_to_cpu(resp->status) == S_OK &&
	       le64_to_cpu(resp->iova) == iova ? 0 : -EIO;
}

/* ------------------------------------------------------------------ */
/* Window coalescing helpers                                           */
/*                                                                     */
/* Caller must hold ad->batch_lock for all of these.                   */
/* ------------------------------------------------------------------ */

static struct dma_window *wnd_lookup(struct apple_dma_dev *ad,
				     phys_addr_t base_gpa, size_t size)
{
	struct dma_window *w;
	u32 bucket = wnd_hash(base_gpa);

	hlist_for_each_entry(w, &ad->window_ht[bucket], hnode) {
		if (w->base_gpa == base_gpa && w->size == size)
			return w;
	}
	return NULL;
}

/*
 * Create a new window by mapping a region of the requested size through the
 * device.  @base_gpa / @size describe the exact reservation we ask the host
 * to set up; the caller decides whether to use a wnd_size-aligned window or
 * an exact-size direct reservation.  Returns the window on success, ERR_PTR
 * on failure.
 *
 * The bookkeeping struct is allocated *before* we ask the host to register
 * the host mapping. Doing it the other way round is dangerous: if the host
 * registration succeeds but a subsequent kmalloc fails, we have to remember
 * to call apple_dma_device_unmap() to undo the host side effect, and a
 * single missing rollback there leaves a stale persistent DMA mapping into
 * guest pages that the kernel has since recycled — i.e. a long-lived
 * silent DMA scribble.  Allocate-first eliminates that whole class of bug.
 */
static struct dma_window *wnd_create(struct apple_dma_dev *ad,
				     phys_addr_t base_gpa, size_t size)
{
	struct dma_window *w;
	u64 iova;
	dma_addr_t dma_addr;
	u32 dma_len;
	int ret;

	w = kmalloc(sizeof(*w), GFP_KERNEL);
	if (!w)
		return ERR_PTR(-ENOMEM);

	ret = apple_dma_device_map(ad, base_gpa, size,
				   &iova, &dma_addr, &dma_len);
	if (ret) {
		kfree(w);
		return ERR_PTR(ret);
	}
	if (dma_len < size) {
		if (apple_dma_device_unmap(ad, iova, size))
			apple_dma_panic_state(&ad->pdev->dev,
					      "incomplete mapping rollback failed");
		kfree(w);
		return ERR_PTR(-EIO);
	}

	w->base_gpa = base_gpa;
	w->size     = size;
	w->dma_addr = dma_addr;
	w->iova     = iova;
	w->refcount = 0;

	hlist_add_head(&w->hnode, &ad->window_ht[wnd_hash(base_gpa)]);
	ad->window_count++;
	return w;
}

/*
 * Decrement a window's refcount. If it drops to zero, unmap the window
 * from the device and free it. Returns true if the window was destroyed.
 *
 * Caller must hold ad->batch_lock and must guarantee w->refcount > 0
 * on entry. Calling this on a window with refcount == 0 underflows
 * the unsigned counter and silently leaks both the struct and the
 * host mapping, which then becomes a hidden persistent DMA
 * mapping into freed guest pages — an extremely effective way to
 * scribble random kernel memory long after the original mapping was
 * "torn down". The WARN_ON catches the regression noisily.
 */
static bool wnd_put(struct apple_dma_dev *ad, struct dma_window *w)
{
	if (WARN_ON_ONCE(w->refcount == 0))
		return false;

	if (--w->refcount > 0)
		return false;

	if (apple_dma_device_unmap(ad, w->iova, w->size)) {
		w->refcount = 1;
		apple_dma_panic_state(&ad->pdev->dev, "host DMA unmap failed");
	}
	hlist_del(&w->hnode);
	ad->window_count--;
	kfree(w);
	return true;
}

/* ------------------------------------------------------------------ */
/* Mapping tracking                                                    */
/* ------------------------------------------------------------------ */

/*
 * Allocate a tracking entry up front, before any host-visible side effects.
 * The map fast path uses this as the first step so that a kmalloc failure
 * here returns -ENOMEM cleanly without ever touching the window hash table
 * or the host mapping. Returns NULL on allocation failure.
 *
 * Caller must subsequently either:
 *   - publish via apple_dma_publish_entry() once a window is in hand, or
 *   - kfree() the entry if some other later step (e.g. wnd_create) fails.
 */
static struct apple_dma_map_entry *apple_dma_alloc_entry(void)
{
	return kmalloc(sizeof(struct apple_dma_map_entry), GFP_KERNEL);
}

/*
 * Publish a pre-allocated tracking entry. Cannot fail: takes maps_lock,
 * adds to the maps list, bumps the active-mapping counters. Caller must
 * already hold a reference on @window (i.e. window->refcount has been
 * incremented for this entry).
 */
static void apple_dma_publish_entry(struct apple_dma_dev *ad,
				    struct apple_dma_map_entry *e,
				    dma_addr_t mapped_dma,
				    dma_addr_t orig_dma, size_t size,
				    struct dma_window *window)
{
	unsigned long flags;

	e->mapped_dma = mapped_dma;
	e->orig_dma   = orig_dma;
	e->size       = size;
	e->window     = window;

	spin_lock_irqsave(&ad->maps_lock, flags);
	list_add_tail(&e->node, &ad->maps);
	spin_unlock_irqrestore(&ad->maps_lock, flags);
	atomic_inc(&ad->map_count);
	atomic64_add(size, &ad->map_bytes);
}

static struct apple_dma_map_entry *
apple_dma_take(struct apple_dma_dev *ad, dma_addr_t mapped_dma, size_t size)
{
	struct apple_dma_map_entry *e, *found = NULL;
	unsigned long flags;

	spin_lock_irqsave(&ad->maps_lock, flags);
	list_for_each_entry(e, &ad->maps, node) {
		if (e->mapped_dma == mapped_dma && e->size == size) {
			list_del(&e->node);
			found = e;
			break;
		}
	}
	spin_unlock_irqrestore(&ad->maps_lock, flags);
	if (found) {
		atomic_dec(&ad->map_count);
		atomic64_sub(found->size, &ad->map_bytes);
	}
	return found;
}

static void apple_dma_free_all(struct apple_dma_dev *ad)
{
	struct apple_dma_map_entry *e, *tmp;
	struct apple_dma_sg_mapping *sg_map, *sg_tmp;
	struct dma_window *w;
	struct hlist_node *htmp;
	LIST_HEAD(stale);
	LIST_HEAD(stale_sg_maps);
	unsigned long flags;
	unsigned int released = 0;
	int i;

	spin_lock_irqsave(&ad->maps_lock, flags);
	list_splice_init(&ad->maps, &stale);
	list_splice_init(&ad->sg_maps, &stale_sg_maps);
	spin_unlock_irqrestore(&ad->maps_lock, flags);

	list_for_each_entry_safe(e, tmp, &stale, node) {
		list_del(&e->node);
		kfree(e);
	}

	list_for_each_entry_safe(sg_map, sg_tmp, &stale_sg_maps, node) {
		list_del(&sg_map->node);
		kfree(sg_map->mapped_dma_addrs);
		kfree(sg_map->orig_dma_addrs);
		kfree(sg_map->orig_dma_lens);
		kfree(sg_map->sizes);
		kfree(sg_map);
	}

	/*
	 * Tear down any remaining windows.  It is critical that we call
	 * apple_dma_device_unmap() on each one: the host (dext) enforces a
	 * "iova is unique" invariant and will reject duplicate registers
	 * with kIOReturnStillOpen. If we skip the unmap here, stale host
	 * entries persist on the host across a module unload/rebind and the
	 * next load will hit "duplicate iova" failures for any GPA that
	 * happens to coincide with a previously-mapped one.
	 */
	mutex_lock(&ad->batch_lock);
	for (i = 0; i < DMA_WINDOW_HT_SIZE; i++) {
		hlist_for_each_entry_safe(w, htmp, &ad->window_ht[i], hnode) {
			/*
			 * We can still talk to the device here: free_all runs
			 * from apple_dma_release_resources() *before* the BAR
			 * is pci_iounmap'd.  ad->ready was cleared in
			 * apple_dma_remove() just to gate new map ops via the
			 * DMA hooks; the command ring itself is still live.
			 */
			if (!ad->bar ||
			    apple_dma_device_unmap(ad, w->iova, w->size)) {
				apple_dma_panic_state(&ad->pdev->dev,
						      "teardown DMA unmap failed");
			}
			hlist_del(&w->hnode);
			ad->window_count--;
			kfree(w);
			released++;
		}
	}
	mutex_unlock(&ad->batch_lock);

	if (released)
		dev_info(&ad->pdev->dev,
			 "apple_dma: released %u host DMA mappings on teardown\n",
			 released);
}


/*
 * Look up the original DMA address for a device-mapped address.
 * Used by sync ops to pass through to the original DMA subsystem.
 * Returns mapped_dma unchanged if no tracking entry is found.
 */
static dma_addr_t apple_dma_orig_addr(struct apple_dma_dev *ad,
				      dma_addr_t mapped_dma)
{
	struct apple_dma_map_entry *e;
	dma_addr_t orig = mapped_dma;
	unsigned long flags;

	spin_lock_irqsave(&ad->maps_lock, flags);
	list_for_each_entry(e, &ad->maps, node) {
		if (e->mapped_dma == mapped_dma) {
			orig = e->orig_dma;
			break;
		}
	}
	spin_unlock_irqrestore(&ad->maps_lock, flags);
	return orig;
}

/* ------------------------------------------------------------------ */
/* Single-entry map helper                                             */
/* ------------------------------------------------------------------ */

/*
 * Pick the window shape for @gpa / @size:
 *   - If windowing is enabled and the mapping fits entirely inside one
 *     wnd_size-aligned window, use (gpa & wnd_mask, wnd_size).  Other
 *     mappings that land in the same window will share this host
 *     reservation.
 *   - Otherwise, use an exact (gpa, size) window.  Duplicates are still
 *     refcounted but nothing else can share it.
 */
static void wnd_shape_for(phys_addr_t gpa, size_t size,
			  phys_addr_t *out_base, size_t *out_size)
{
	if (wnd_enabled() && size <= wnd_size) {
		phys_addr_t base = gpa & wnd_mask;
		phys_addr_t end_base = (gpa + size - 1) & wnd_mask;

		if (base == end_base) {
			*out_base = base;
			*out_size = wnd_size;
			return;
		}
	}

	*out_base = gpa;
	*out_size = size;
}

/*
 * Map a single region, routing through the window coalescing layer.
 * Caller must hold ad->batch_lock.
 *
 * Ordering invariant (important for correctness, not just style):
 *   1. Allocate the tracking entry first.
 *   2. Look up or create the host-side window.
 *   3. Increment window refcount.
 *   4. Publish the tracking entry — this step is infallible.
 *
 * Steps 1, 2 and 3 are all reversible in isolation (kfree the entry,
 * wnd_put the window if we created it, no-op for a hash hit).  The
 * irreversible publish only happens once everything that can fail has
 * succeeded.  This means the map path has no half-built error state
 * that could leak a host DMA mapping behind a freed guest page if a
 * late kmalloc fails — the historical shape of this function had
 * exactly that bug.
 */
static int apple_dma_map_locked(struct apple_dma_dev *ad, struct device *dev,
				phys_addr_t gpa,
				size_t size, dma_addr_t orig_dma,
				dma_addr_t *out_dma)
{
	phys_addr_t base;
	size_t wsize;
	struct dma_window *w;
	struct apple_dma_map_entry *e;
	dma_addr_t dma_addr;

	if (!size || size > U32_MAX || gpa > (phys_addr_t)-1 - (size - 1))
		return -E2BIG;

	e = apple_dma_alloc_entry();
	if (!e)
		return -ENOMEM;

	wnd_shape_for(gpa, size, &base, &wsize);
	if (wsize > U32_MAX) {
		kfree(e);
		return -E2BIG;
	}

	w = wnd_lookup(ad, base, wsize);
	if (!w) {
		w = wnd_create(ad, base, wsize);
		if (IS_ERR(w)) {
			kfree(e);
			return PTR_ERR(w);
		}
	}

	dma_addr = w->dma_addr + (gpa - base);
	w->refcount++;
	if (dma_addr == DMA_MAPPING_ERROR || dma_addr > dma_get_mask(dev) ||
	    size - 1 > dma_get_mask(dev) - dma_addr) {
		wnd_put(ad, w);
		kfree(e);
		return -ERANGE;
	}

	apple_dma_publish_entry(ad, e, dma_addr, orig_dma, size, w);

	*out_dma = dma_addr;
	return 0;
}

static int apple_dma_map_one(struct apple_dma_dev *ad, struct device *dev,
			     phys_addr_t gpa, size_t size,
			     dma_addr_t orig_dma, dma_addr_t *out_dma)
{
	int ret;

	if (!apple_dma_usable(ad))
		apple_dma_panic_state(dev, "map requested in unusable context");

	if (orig_dma != gpa) {
		dev_warn(dev, "apple_dma: original DMA mapping is not identity; "
			 "bounce buffers and guest IOMMUs are unsupported\n");
		return -EOPNOTSUPP;
	}

	mutex_lock(&ad->batch_lock);
	ret = apple_dma_map_locked(ad, dev, gpa, size, orig_dma, out_dma);
	mutex_unlock(&ad->batch_lock);

	if (ret) {
		dev_warn(dev,
			 "apple_dma: map failed gpa=%pap size=%zu ret=%d"
			 " (active: %u maps, %llu bytes)\n",
			 &gpa, size, ret,
			 atomic_read(&ad->map_count),
			 (u64)atomic64_read(&ad->map_bytes));

		return ret;
	}

	apple_dma_trace_record(ad, APPLE_DMA_TRACE_OP_MAP, gpa, size);
	return 0;
}

/* ------------------------------------------------------------------ */
/* DMA ops — map_phys / map_page                                       */
/* ------------------------------------------------------------------ */

/*
 * Release a single tracked mapping entry. Handles windowed and direct
 * entries. Caller must hold ad->batch_lock.
 */
static void apple_dma_release_entry(struct apple_dma_dev *ad,
				    struct device *dev,
				    struct apple_dma_map_entry *e)
{
	(void)dev;
	wnd_put(ad, e->window);
}

/*
 * Common unmap path: take entry, trace, release, return orig_dma.
 * Returns the original DMA address, or @addr if device is not ready.
 */
static dma_addr_t apple_dma_unmap_one(struct apple_dma_dev *ad,
				      struct device *dev, dma_addr_t addr,
				      size_t size, u8 trace_op)
{
	dma_addr_t orig = addr;

	if (ad && ad->ready) {
		struct apple_dma_map_entry *e = apple_dma_take(ad, addr, size);

		if (!e)
			apple_dma_panic_state(dev, "unmap: missing mapping");

		apple_dma_trace_record(ad, trace_op,
				       e->window->base_gpa +
				       (e->mapped_dma - e->window->dma_addr),
				       size);

		mutex_lock(&ad->batch_lock);
		apple_dma_release_entry(ad, dev, e);
		mutex_unlock(&ad->batch_lock);

		orig = e->orig_dma;
		kfree(e);
	}
	return orig;
}

#ifdef APPLE_DMA_HAS_MAP_PHYS
static dma_addr_t apple_dma_map_phys(struct device *dev, phys_addr_t phys,
				     size_t size, enum dma_data_direction dir,
				     unsigned long attrs)
{
	struct apple_dma_dev *ad = apple_dma_find_by_dev(dev);
	dma_addr_t orig, mapped;
	unsigned int count;
	int ret;

	count = apple_dma_trace_stat_inc(APPLE_DMA_STAT_MAP);
	if (apple_dma_trace_should_log(count))
		dev_info(dev, "apple_dma: map_phys #%u phys=%pap size=%zu\n",
			 count, &phys, size);

	if (!ad || !ad->orig_dma_ops)
		orig = apple_dma_orig_map_phys(ad, dev, phys, size, dir, attrs);
	else if (ad->orig_dma_ops->map_phys)
		orig = ad->orig_dma_ops->map_phys(dev, phys, size, dir, attrs);
	else
		return DMA_MAPPING_ERROR;

	if (dma_mapping_error(dev, orig))
		return orig;

	ret = apple_dma_map_one(ad, dev, phys, size, orig, &mapped);
	if (ret) {
		if (!ad || !ad->orig_dma_ops)
			apple_dma_orig_unmap_phys(ad, dev, orig, size, dir, attrs);
		else if (ad->orig_dma_ops->unmap_phys)
			ad->orig_dma_ops->unmap_phys(dev, orig, size, dir,
						     attrs);
		return DMA_MAPPING_ERROR;
	}

	return mapped;
}

static void apple_dma_unmap_phys(struct device *dev, dma_addr_t addr,
				 size_t size, enum dma_data_direction dir,
				 unsigned long attrs)
{
	struct apple_dma_dev *ad = apple_dma_find_by_dev(dev);
	dma_addr_t orig;

	apple_dma_trace_stat_inc(APPLE_DMA_STAT_UNMAP);
	orig = apple_dma_unmap_one(ad, dev, addr, size,
				   APPLE_DMA_TRACE_OP_UNMAP);

	if (!ad || !ad->orig_dma_ops)
		apple_dma_orig_unmap_phys(ad, dev, orig, size, dir, attrs);
	else if (ad->orig_dma_ops->unmap_phys)
		ad->orig_dma_ops->unmap_phys(dev, orig, size, dir, attrs);
}
#else /* !APPLE_DMA_HAS_MAP_PHYS */
static dma_addr_t apple_dma_map_page(struct device *dev, struct page *page,
				     unsigned long offset, size_t size,
				     enum dma_data_direction dir,
				     unsigned long attrs)
{
	struct apple_dma_dev *ad = apple_dma_find_by_dev(dev);
	phys_addr_t phys = page_to_phys(page) + offset;
	dma_addr_t orig, mapped;
	unsigned int count;
	int ret;

	count = apple_dma_trace_stat_inc(APPLE_DMA_STAT_MAP);
	if (apple_dma_trace_should_log(count))
		dev_info(dev, "apple_dma: map_page #%u phys=%pap size=%zu\n",
			 count, &phys, size);

	if (!ad || !ad->orig_dma_ops)
		orig = apple_dma_orig_map_page(ad, dev, page, offset, size, dir,
					       attrs);
	else if (ad->orig_dma_ops->map_page)
		orig = ad->orig_dma_ops->map_page(dev, page, offset, size,
						   dir, attrs);
	else
		return DMA_MAPPING_ERROR;

	if (dma_mapping_error(dev, orig))
		return orig;

	ret = apple_dma_map_one(ad, dev, phys, size, orig, &mapped);
	if (ret) {
		if (!ad || !ad->orig_dma_ops)
			apple_dma_orig_unmap_page(ad, dev, orig, size, dir, attrs);
		else if (ad->orig_dma_ops->unmap_page)
			ad->orig_dma_ops->unmap_page(dev, orig, size, dir,
						     attrs);
		return DMA_MAPPING_ERROR;
	}

	return mapped;
}

static void apple_dma_unmap_page(struct device *dev, dma_addr_t addr,
				 size_t size, enum dma_data_direction dir,
				 unsigned long attrs)
{
	struct apple_dma_dev *ad = apple_dma_find_by_dev(dev);
	dma_addr_t orig;

	apple_dma_trace_stat_inc(APPLE_DMA_STAT_UNMAP);
	orig = apple_dma_unmap_one(ad, dev, addr, size,
				   APPLE_DMA_TRACE_OP_UNMAP);

	if (!ad || !ad->orig_dma_ops)
		apple_dma_orig_unmap_page(ad, dev, orig, size, dir, attrs);
	else if (ad->orig_dma_ops->unmap_page)
		ad->orig_dma_ops->unmap_page(dev, orig, size, dir, attrs);
}
#endif /* APPLE_DMA_HAS_MAP_PHYS */

/* ------------------------------------------------------------------ */
/* DMA ops — map_sg / unmap_sg                                         */
/* ------------------------------------------------------------------ */

static int apple_dma_map_sg(struct device *dev, struct scatterlist *sg,
			    int nents, enum dma_data_direction dir,
			    unsigned long attrs)
{
	struct apple_dma_dev *ad = apple_dma_find_by_dev(dev);
	struct scatterlist *s;
	struct apple_dma_sg_mapping *sg_map;
	unsigned int count;
	unsigned long flags;
	int mapped, i, err_idx = -1, error = -EIO;

	count = apple_dma_trace_stat_inc(APPLE_DMA_STAT_MAP_SG);
	if (apple_dma_trace_should_log(count))
		dev_info(dev, "apple_dma: map_sg #%u nents=%d\n",
			 count, nents);

	if (!ad || !ad->orig_dma_ops) {
		dev_warn(dev, "apple_dma: direct DMA backend cannot be safely wrapped\n");
		return -EIO;
	}

	if (!apple_dma_usable(ad))
		apple_dma_panic_state(dev, "map_sg in unusable context");

	if (!ad->orig_dma_ops->map_sg)
		return -EIO;

	sg_map = kzalloc(sizeof(*sg_map), GFP_KERNEL);
	if (!sg_map)
		return -ENOMEM;
	sg_map->sg = sg;
	sg_map->nents = nents;
	sg_map->mapped_dma_addrs =
		kcalloc(nents, sizeof(*sg_map->mapped_dma_addrs), GFP_KERNEL);
	sg_map->orig_dma_addrs =
		kcalloc(nents, sizeof(*sg_map->orig_dma_addrs), GFP_KERNEL);
	sg_map->orig_dma_lens =
		kcalloc(nents, sizeof(*sg_map->orig_dma_lens), GFP_KERNEL);
	sg_map->sizes = kcalloc(nents, sizeof(*sg_map->sizes), GFP_KERNEL);
	if (!sg_map->mapped_dma_addrs || !sg_map->orig_dma_addrs ||
	    !sg_map->orig_dma_lens || !sg_map->sizes) {
		kfree(sg_map->mapped_dma_addrs);
		kfree(sg_map->orig_dma_addrs);
		kfree(sg_map->orig_dma_lens);
		kfree(sg_map->sizes);
		kfree(sg_map);
		return -ENOMEM;
	}

	spin_lock_irqsave(&ad->maps_lock, flags);
	if (!list_empty(&ad->sg_maps)) {
		struct apple_dma_sg_mapping *existing;

		list_for_each_entry(existing, &ad->sg_maps, node) {
			if (existing->sg == sg) {
				spin_unlock_irqrestore(&ad->maps_lock, flags);
				dev_warn(dev, "apple_dma: duplicate map_sg scatterlist\n");
				goto err_free_sg_map;
			}
		}
	}
	list_add_tail(&sg_map->node, &ad->sg_maps);
	spin_unlock_irqrestore(&ad->maps_lock, flags);

	for_each_sg(sg, s, nents, i)
		sg_map->sizes[i] = s->length;

	mapped = ad->orig_dma_ops->map_sg(dev, sg, nents, dir, attrs);
	if (mapped <= 0) {
		if (mapped < 0)
			dev_warn(dev, "apple_dma: underlying map_sg returned %d\n",
				 mapped);
		if (mapped == -ENOMEM || mapped == -EINVAL ||
		    mapped == -EREMOTEIO)
			error = mapped;
		goto err_remove_sg_map;
	}
	if (mapped > nents) {
		if (ad->orig_dma_ops->unmap_sg)
			ad->orig_dma_ops->unmap_sg(dev, sg, nents, dir, attrs);
		else
			apple_dma_orig_unmap_sg(ad, dev, sg, nents, dir, attrs);
		goto err_remove_sg_map;
	}
	sg_map->orig_mapped = mapped;

	for_each_sg(sg, s, nents, i) {
		sg_map->orig_dma_addrs[i] = sg_dma_address(s);
		sg_map->orig_dma_lens[i] = sg_dma_len(s);
	}

	/*
	 * Mapping the original pages is unsafe if the guest DMA backend used
	 * bounce buffers, an IOMMU, or merged physically discontiguous entries.
	 */
	if (mapped != nents) {
		dev_warn(dev, "apple_dma: coalesced guest SG mappings unsupported\n");
		goto err_unmap_orig;
	}
	for_each_sg(sg, s, nents, i) {
		if (!sg_map->sizes[i] ||
		    sg_map->sizes[i] > dma_get_max_seg_size(dev) ||
		    sg_map->orig_dma_addrs[i] != sg_phys(s) ||
		    sg_map->orig_dma_lens[i] != sg_map->sizes[i]) {
			dev_warn(dev, "apple_dma: non-identity guest SG mapping unsupported\n");
			goto err_unmap_orig;
		}
	}

	mutex_lock(&ad->batch_lock);

	for_each_sg(sg, s, nents, i) {
		phys_addr_t gpa = sg_phys(s);
		size_t size = sg_map->sizes[i];
		dma_addr_t out_dma;
		int ret;

		ret = apple_dma_map_locked(ad, dev, gpa, size,
					   sg_map->orig_dma_addrs[i], &out_dma);
		if (ret) {
			dev_warn(dev,
				 "apple_dma: map_sg entry %d/%d failed"
				 " gpa=%pap size=%zu ret=%d"
				 " (active: %u maps, %llu bytes)\n",
				 i, nents, &gpa, size, ret,
				 atomic_read(&ad->map_count),
				 (u64)atomic64_read(&ad->map_bytes));
			err_idx = i;
			if (ret == -ENOMEM)
				error = ret;
			break;
		}
		sg_map->mapped_dma_addrs[i] = out_dma;
		if ((out_dma & ~(dma_addr_t)dma_get_seg_boundary(dev)) !=
		    ((out_dma + size - 1) &
		     ~(dma_addr_t)dma_get_seg_boundary(dev))) {
			dev_warn(dev, "apple_dma: mapped SG segment crosses device boundary\n");
			err_idx = i + 1;
			break;
		}
		apple_dma_trace_record(ad, APPLE_DMA_TRACE_OP_MAP_SG, gpa, size);
	}

	mutex_unlock(&ad->batch_lock);

	if (err_idx >= 0) {
		/* Roll back host mappings created for this scatterlist. */
		int j;

		for_each_sg(sg, s, err_idx, j) {
			struct apple_dma_map_entry *e;

			e = apple_dma_take(ad, sg_map->mapped_dma_addrs[j],
					   sg_map->sizes[j]);
			if (!e)
				apple_dma_panic_state(dev,
						      "map_sg rollback: missing mapping");
			mutex_lock(&ad->batch_lock);
			apple_dma_release_entry(ad, dev, e);
			mutex_unlock(&ad->batch_lock);
			kfree(e);
		}

		for_each_sg(sg, s, nents, j) {
			sg_dma_address(s) = sg_map->orig_dma_addrs[j];
			sg_dma_len(s) = sg_map->orig_dma_lens[j];
		}
		goto err_unmap_orig;
	}

	/* Preserve each physical input range; don't reuse coalesced DMA lengths. */
	for_each_sg(sg, s, nents, i) {
		sg_dma_address(s) = sg_map->mapped_dma_addrs[i];
		sg_dma_len(s) = sg_map->sizes[i];
	}

	return nents;

err_unmap_orig:
	if (ad->orig_dma_ops->unmap_sg)
		ad->orig_dma_ops->unmap_sg(dev, sg, nents, dir, attrs);
	else
		apple_dma_orig_unmap_sg(ad, dev, sg, nents, dir, attrs);
err_remove_sg_map:
	spin_lock_irqsave(&ad->maps_lock, flags);
	list_del(&sg_map->node);
	spin_unlock_irqrestore(&ad->maps_lock, flags);
err_free_sg_map:
	kfree(sg_map->mapped_dma_addrs);
	kfree(sg_map->orig_dma_addrs);
	kfree(sg_map->orig_dma_lens);
	kfree(sg_map->sizes);
	kfree(sg_map);
	return error;
}

static void apple_dma_unmap_sg(struct device *dev, struct scatterlist *sg,
			       int nents, enum dma_data_direction dir,
			       unsigned long attrs)
{
	struct apple_dma_dev *ad = apple_dma_find_by_dev(dev);
	struct scatterlist *s;
	struct apple_dma_sg_mapping *sg_map = NULL, *candidate;
	unsigned int count;
	unsigned long flags;
	int i;

	count = apple_dma_trace_stat_inc(APPLE_DMA_STAT_UNMAP_SG);
	if (apple_dma_trace_should_log(count))
		dev_info(dev, "apple_dma: unmap_sg #%u nents=%d\n",
			 count, nents);

	if (ad && ad->ready) {
		spin_lock_irqsave(&ad->maps_lock, flags);
		list_for_each_entry(candidate, &ad->sg_maps, node) {
			if (candidate->sg == sg) {
				sg_map = candidate;
				list_del(&sg_map->node);
				break;
			}
		}
		spin_unlock_irqrestore(&ad->maps_lock, flags);
		if (!sg_map || sg_map->nents != nents)
			apple_dma_panic_state(dev,
					      "unmap_sg: missing mapping record");

		for_each_sg(sg, s, sg_map->nents, i) {
			struct apple_dma_map_entry *e;

			e = apple_dma_take(ad, sg_map->mapped_dma_addrs[i],
					   sg_map->sizes[i]);
			if (!e)
				apple_dma_panic_state(dev,
						      "unmap_sg: missing mapping");
			apple_dma_trace_record(ad, APPLE_DMA_TRACE_OP_UNMAP_SG,
					       e->window->base_gpa +
					       (e->mapped_dma -
						e->window->dma_addr),
					       e->size);
			mutex_lock(&ad->batch_lock);
			apple_dma_release_entry(ad, dev, e);
			mutex_unlock(&ad->batch_lock);
			kfree(e);
		}
		for_each_sg(sg, s, sg_map->nents, i) {
			sg_dma_address(s) = sg_map->orig_dma_addrs[i];
			sg_dma_len(s) = sg_map->orig_dma_lens[i];
		}
		kfree(sg_map->mapped_dma_addrs);
		kfree(sg_map->orig_dma_addrs);
		kfree(sg_map->orig_dma_lens);
		kfree(sg_map->sizes);
		kfree(sg_map);
	}

	if (!ad || !ad->orig_dma_ops)
		apple_dma_orig_unmap_sg(ad, dev, sg, nents, dir, attrs);
	else if (ad->orig_dma_ops->unmap_sg)
		ad->orig_dma_ops->unmap_sg(dev, sg, nents, dir, attrs);

}

/* ------------------------------------------------------------------ */
/* DMA ops — alloc / free (coherent DMA buffers)                       */
/* ------------------------------------------------------------------ */

static int apple_dma_remap_alloc(struct apple_dma_dev *ad, struct device *dev,
				 phys_addr_t phys, size_t size,
				 dma_addr_t orig_dma, dma_addr_t *dma_handle)
{
	dma_addr_t mapped_dma;
	int ret;

	ret = apple_dma_map_one(ad, dev, phys, size, orig_dma, &mapped_dma);
	if (ret)
		return ret;

	if (mapped_dma > dev->coherent_dma_mask ||
	    size - 1 > dev->coherent_dma_mask - mapped_dma) {
		apple_dma_unmap_one(ad, dev, mapped_dma, size,
				    APPLE_DMA_TRACE_OP_FREE);
		dev_warn(dev, "apple_dma: mapped allocation exceeds coherent DMA mask\n");
		return -ERANGE;
	}

	*dma_handle = mapped_dma;
	return 0;
}

static void *apple_dma_alloc(struct device *dev, size_t size,
			     dma_addr_t *dma_handle, gfp_t gfp,
			     unsigned long attrs)
{
	struct apple_dma_dev *ad = apple_dma_find_by_dev(dev);
	dma_addr_t orig_dma;
	void *cpu_addr;
	phys_addr_t phys;
	unsigned int count;

	count = apple_dma_trace_stat_inc(APPLE_DMA_STAT_ALLOC);
	if (apple_dma_trace_should_log(count))
		dev_info(dev, "apple_dma: alloc #%u size=%zu\n",
			 count, size);

	cpu_addr = apple_dma_orig_alloc_attrs(ad, dev, size, &orig_dma,
					      gfp, attrs);
	if (!cpu_addr)
		return NULL;

	if (!virt_addr_valid(cpu_addr)) {
		dev_warn(dev, "apple_dma: coherent allocation is not directly mapped\n");
		apple_dma_orig_free_attrs(ad, dev, size, cpu_addr, orig_dma, attrs);
		return NULL;
	}

	phys = virt_to_phys(cpu_addr);
	if (apple_dma_remap_alloc(ad, dev, phys, size, orig_dma, dma_handle)) {
		apple_dma_orig_free_attrs(ad, dev, size, cpu_addr, orig_dma, attrs);
		return NULL;
	}

	return cpu_addr;
}

static void apple_dma_free(struct device *dev, size_t size, void *cpu_addr,
			   dma_addr_t dma_addr, unsigned long attrs)
{
	struct apple_dma_dev *ad = apple_dma_find_by_dev(dev);
	dma_addr_t orig_dma;

	apple_dma_trace_stat_inc(APPLE_DMA_STAT_FREE);
	orig_dma = apple_dma_unmap_one(ad, dev, dma_addr, size,
				       APPLE_DMA_TRACE_OP_FREE);
	apple_dma_orig_free_attrs(ad, dev, size, cpu_addr, orig_dma, attrs);
}

static struct page *apple_dma_alloc_pages(struct device *dev, size_t size,
					  dma_addr_t *dma_handle,
					  enum dma_data_direction dir,
					  gfp_t gfp)
{
	struct apple_dma_dev *ad = apple_dma_find_by_dev(dev);
	dma_addr_t orig_dma;
	struct page *page;
	phys_addr_t phys;
	unsigned int count;

	count = apple_dma_trace_stat_inc(APPLE_DMA_STAT_ALLOC);
	if (apple_dma_trace_should_log(count))
		dev_info(dev, "apple_dma: alloc_pages #%u size=%zu\n",
			 count, size);

	page = apple_dma_orig_alloc_pages(ad, dev, size, &orig_dma, dir, gfp);
	if (!page)
		return NULL;

	phys = page_to_phys(page);
	if (apple_dma_remap_alloc(ad, dev, phys, size, orig_dma, dma_handle)) {
		apple_dma_orig_free_pages(ad, dev, size, page, orig_dma, dir);
		return NULL;
	}

	return page;
}

static void apple_dma_free_pages(struct device *dev, size_t size,
				 struct page *page, dma_addr_t dma_addr,
				 enum dma_data_direction dir)
{
	struct apple_dma_dev *ad = apple_dma_find_by_dev(dev);
	dma_addr_t orig_dma;

	apple_dma_trace_stat_inc(APPLE_DMA_STAT_FREE);
	orig_dma = apple_dma_unmap_one(ad, dev, dma_addr, size,
				       APPLE_DMA_TRACE_OP_FREE);
	apple_dma_orig_free_pages(ad, dev, size, page, orig_dma, dir);
}

/* ------------------------------------------------------------------ */
/* DMA ops — sync (pass through to original ops)                       */
/* ------------------------------------------------------------------ */

typedef void (*apple_dma_sync_fn_t)(struct device *dev, dma_addr_t dma_handle,
				    size_t size,
				    enum dma_data_direction dir);
typedef void (*apple_dma_sync_sg_fn_t)(struct device *dev,
				       struct scatterlist *sg, int nents,
				       enum dma_data_direction dir);

static dma_addr_t apple_dma_translate_dma_handle(struct apple_dma_dev *ad,
						 dma_addr_t dma_handle)
{
	if (ad && ad->ready)
		return apple_dma_orig_addr(ad, dma_handle);
	return dma_handle;
}

static void apple_dma_sync_single_common(struct apple_dma_dev *ad,
					 struct device *dev,
					 dma_addr_t dma_handle, size_t size,
					 enum dma_data_direction dir,
					 apple_dma_sync_fn_t sync_fn)
{
	if (!sync_fn)
		return;

	sync_fn(dev, apple_dma_translate_dma_handle(ad, dma_handle), size, dir);
}

static void apple_dma_sync_sg_common(struct apple_dma_dev *ad,
				     struct device *dev,
				     struct scatterlist *sg, int nents,
				     enum dma_data_direction dir,
				     apple_dma_sync_fn_t sync_fn,
				     apple_dma_sync_sg_fn_t sync_sg_fn)
{
	struct scatterlist *s;
	struct apple_dma_sg_mapping *sg_map = NULL, *candidate;
	unsigned long flags;
	int i;

	if (ad && ad->ready) {
		spin_lock_irqsave(&ad->maps_lock, flags);
		list_for_each_entry(candidate, &ad->sg_maps, node) {
			if (candidate->sg == sg) {
				sg_map = candidate;
				break;
			}
		}
		spin_unlock_irqrestore(&ad->maps_lock, flags);
	}

	if (sg_map) {
		for_each_sg(sg, s, sg_map->nents, i) {
			sg_dma_address(s) = sg_map->orig_dma_addrs[i];
			sg_dma_len(s) = sg_map->orig_dma_lens[i];
		}
		if (sync_sg_fn) {
			sync_sg_fn(dev, sg, sg_map->nents, dir);
		} else if (sync_fn) {
			for_each_sg(sg, s, sg_map->orig_mapped, i)
				sync_fn(dev, sg_dma_address(s), sg_dma_len(s), dir);
		}
		for_each_sg(sg, s, sg_map->nents, i) {
			sg_dma_address(s) = sg_map->mapped_dma_addrs[i];
			sg_dma_len(s) = sg_map->sizes[i];
		}
		return;
	}

	if (!sync_fn)
		return;

	for_each_sg(sg, s, nents, i) {
		sync_fn(dev,
			apple_dma_translate_dma_handle(ad, sg_dma_address(s)),
			sg_dma_len(s), dir);
	}
}

static void apple_dma_sync_single_for_cpu(struct device *dev,
					  dma_addr_t dma_handle, size_t size,
					  enum dma_data_direction dir)
{
	struct apple_dma_dev *ad = apple_dma_find_by_dev(dev);

	apple_dma_sync_single_common(ad, dev, dma_handle, size, dir,
				     ad && ad->orig_dma_ops ?
				     ad->orig_dma_ops->sync_single_for_cpu :
				     NULL);
}

static void apple_dma_sync_single_for_device(struct device *dev,
					     dma_addr_t dma_handle, size_t size,
					     enum dma_data_direction dir)
{
	struct apple_dma_dev *ad = apple_dma_find_by_dev(dev);

	apple_dma_sync_single_common(ad, dev, dma_handle, size, dir,
				     ad && ad->orig_dma_ops ?
				     ad->orig_dma_ops->sync_single_for_device :
				     NULL);
}

static void apple_dma_sync_sg_for_cpu(struct device *dev,
				      struct scatterlist *sg, int nents,
				      enum dma_data_direction dir)
{
	struct apple_dma_dev *ad = apple_dma_find_by_dev(dev);

	apple_dma_sync_sg_common(ad, dev, sg, nents, dir,
				 ad && ad->orig_dma_ops ?
				 ad->orig_dma_ops->sync_single_for_cpu : NULL,
				 ad && ad->orig_dma_ops ?
				 ad->orig_dma_ops->sync_sg_for_cpu : NULL);
}

static void apple_dma_sync_sg_for_device(struct device *dev,
					 struct scatterlist *sg, int nents,
					 enum dma_data_direction dir)
{
	struct apple_dma_dev *ad = apple_dma_find_by_dev(dev);

	apple_dma_sync_sg_common(ad, dev, sg, nents, dir,
				 ad && ad->orig_dma_ops ?
				 ad->orig_dma_ops->sync_single_for_device :
				 NULL,
				 ad && ad->orig_dma_ops ?
				 ad->orig_dma_ops->sync_sg_for_device : NULL);
}

static const struct dma_map_ops apple_dma_ops = {
	.alloc			= apple_dma_alloc,
	.free			= apple_dma_free,
	.APPLE_DMA_ALLOC_PAGES_FIELD = apple_dma_alloc_pages,
	.free_pages		= apple_dma_free_pages,
#ifdef APPLE_DMA_HAS_MAP_PHYS
	.map_phys		= apple_dma_map_phys,
	.unmap_phys		= apple_dma_unmap_phys,
#else
	.map_page		= apple_dma_map_page,
	.unmap_page		= apple_dma_unmap_page,
#endif
	.map_sg			= apple_dma_map_sg,
	.unmap_sg		= apple_dma_unmap_sg,
	.sync_single_for_cpu	= apple_dma_sync_single_for_cpu,
	.sync_single_for_device	= apple_dma_sync_single_for_device,
	.sync_sg_for_cpu	= apple_dma_sync_sg_for_cpu,
	.sync_sg_for_device	= apple_dma_sync_sg_for_device,
};

/* ------------------------------------------------------------------ */
/* Managed passthrough PCI device (hook dma_ops)                        */
/* ------------------------------------------------------------------ */

static int apple_dma_bind_managed_pci(struct apple_dma_dev *ad)
{
	u8 bus = (u8)(ad->managed_bdf >> 8);
	u8 devfn = (u8)(ad->managed_bdf & 0xff);
	struct pci_dev *mpdev;

	mpdev = pci_get_domain_bus_and_slot(0, bus, devfn);
	if (!mpdev) {
		dev_err(&ad->pdev->dev,
			"managed PCI device %02x:%02x.%u not found\n",
			bus, PCI_SLOT(devfn), PCI_FUNC(devfn));
		return -ENODEV;
	}

	if ((managed_vendor != 0xffff && mpdev->vendor != managed_vendor) ||
	    (managed_device != 0xffff && mpdev->device != managed_device)) {
		dev_err(&ad->pdev->dev,
			"managed device %s doesn't match filter %04x:%04x\n",
			pci_name(mpdev), managed_vendor, managed_device);
		pci_dev_put(mpdev);
		return -ENODEV;
	}

	ad->managed_pdev = mpdev;
	ad->orig_dma_ops = get_dma_ops(&mpdev->dev);
	set_dma_ops(&mpdev->dev, &apple_dma_ops);

	dev_info(&ad->pdev->dev, "bound to managed endpoint %s\n",
		 pci_name(mpdev));
	return 0;
}

static void apple_dma_unbind_managed_pci(struct apple_dma_dev *ad)
{
	if (!ad->managed_pdev)
		return;
	set_dma_ops(&ad->managed_pdev->dev, ad->orig_dma_ops);
	pci_dev_put(ad->managed_pdev);
	ad->managed_pdev = NULL;
}

static void apple_dma_remove_instance(struct apple_dma_dev *ad)
{
	mutex_lock(&instances_lock);
	list_del(&ad->instance_node);
	mutex_unlock(&instances_lock);
	pci_set_drvdata(ad->pdev, NULL);
}

static void apple_dma_release_resources(struct pci_dev *pdev,
					struct apple_dma_dev *ad)
{
#ifdef CONFIG_ARM64
	if (ad->uvm_page_patched) {
		mutex_lock(&instances_lock);
		if (--uvm_page_refcount == 0)
			uvm_page_patch_exit();
		mutex_unlock(&instances_lock);
	}
	if (ad->dczid_patched) {
		mutex_lock(&instances_lock);
		if (--dczid_refcount == 0)
			dczid_patch_revert();
		mutex_unlock(&instances_lock);
	}
#endif
	apple_dma_unbind_managed_pci(ad);
	apple_dma_free_all(ad);
	kfree(ad->cmd_page);
	if (ad->bar)
		pci_iounmap(pdev, ad->bar);
	pci_release_regions(pdev);
	pci_disable_device(pdev);
}

/* ------------------------------------------------------------------ */
/* PCI driver                                                          */
/* ------------------------------------------------------------------ */

static int apple_dma_probe(struct pci_dev *pdev,
			   const struct pci_device_id *id)
{
	struct apple_dma_dev *ad;
	u32 version;
	phys_addr_t cmd_phys;
	int ret;

	ad = kzalloc(sizeof(*ad), GFP_KERNEL);
	if (!ad)
		return -ENOMEM;

	ad->pdev = pdev;
	INIT_LIST_HEAD(&ad->instance_node);
	mutex_init(&ad->batch_lock);
	spin_lock_init(&ad->maps_lock);
	INIT_LIST_HEAD(&ad->maps);
	INIT_LIST_HEAD(&ad->sg_maps);

	ret = pci_enable_device(pdev);
	if (ret)
		goto err_free;

	ret = pci_request_regions(pdev, "apple-dma");
	if (ret)
		goto err_disable;

	ad->bar = pci_iomap(pdev, 0, 0);
	if (!ad->bar) {
		ret = -ENOMEM;
		goto err_release;
	}

	version = ioread32(ad->bar + REG_VERSION);
	if (version != APPLE_DMA_VERSION) {
		dev_err(&pdev->dev, "unsupported version %u (expected %u)\n",
			version, APPLE_DMA_VERSION);
		ret = -EINVAL;
		goto err_release;
	}

	ad->managed_bdf = (u16)ioread32(ad->bar + REG_MANAGED_BDF);
	ad->max_entries = ioread32(ad->bar + REG_MAX_ENTRIES);

	if (!ad->max_entries) {
		dev_err(&pdev->dev, "device reports max_entries=0\n");
		ret = -EINVAL;
		goto err_release;
	}

	ad->cmd_page = kzalloc(sizeof(*ad->cmd_page), GFP_KERNEL);
	if (!ad->cmd_page) {
		ret = -ENOMEM;
		goto err_release;
	}

	/* Tell the device where our command page lives */
	cmd_phys = virt_to_phys(ad->cmd_page);
	iowrite32(lower_32_bits(cmd_phys), ad->bar + REG_CMD_GPA_LO);
	iowrite32(upper_32_bits(cmd_phys), ad->bar + REG_CMD_GPA_HI);

	pci_set_drvdata(pdev, ad);

	ret = apple_dma_bind_managed_pci(ad);
	if (ret)
		goto err_release;

#ifdef CONFIG_ARM64
	if (!enable_quirks) {
		dev_info(&pdev->dev, "vendor quirks disabled by module parameter\n");
	} else {
		if (ad->managed_pdev->vendor == PCI_VENDOR_ID_ATI) {
			mutex_lock(&instances_lock);
			if (dczid_refcount++ == 0) {
				ret = dczid_patch_apply();
				if (ret) {
					dczid_refcount--;
					mutex_unlock(&instances_lock);
					goto err_release;
				}
			}
			mutex_unlock(&instances_lock);
			ad->dczid_patched = true;
		}

		if (ad->managed_pdev->vendor == PCI_VENDOR_ID_NVIDIA) {
			mutex_lock(&instances_lock);
			if (uvm_page_refcount++ == 0) {
				ret = uvm_page_patch_init();
				if (ret) {
					uvm_page_refcount--;
					mutex_unlock(&instances_lock);
					goto err_release;
				}
			}
			mutex_unlock(&instances_lock);
			ad->uvm_page_patched = true;
		}
	}
#else
	if (enable_quirks || enable_tso)
		dev_info(&pdev->dev,
			 "ARM-specific quirks are unavailable on this architecture\n");
#endif

	/* Add to instance list and mark ready */
	mutex_lock(&instances_lock);
	list_add_tail(&ad->instance_node, &apple_dma_instances);
	mutex_unlock(&instances_lock);

	ad->ready = true;

	dev_info(&pdev->dev,
		 "ready: managed_bdf=%04x max_entries=%u\n",
		 ad->managed_bdf, ad->max_entries);
	return 0;

err_release:
	pci_set_drvdata(pdev, NULL);
	apple_dma_release_resources(pdev, ad);
err_free:
	kfree(ad);
	return ret;
err_disable:
	pci_disable_device(pdev);
	goto err_free;
}

static void apple_dma_remove(struct pci_dev *pdev)
{
	struct apple_dma_dev *ad = pci_get_drvdata(pdev);

	if (!ad)
		return;

	ad->ready = false;
	apple_dma_remove_instance(ad);
	apple_dma_release_resources(pdev, ad);
	kfree(ad);
}

static const struct pci_device_id apple_dma_pci_ids[] = {
	{ PCI_DEVICE(APPLE_DMA_PCI_VENDOR, APPLE_DMA_PCI_DEVICE) },
	{ 0 }
};
MODULE_DEVICE_TABLE(pci, apple_dma_pci_ids);

static struct pci_driver apple_dma_pci_driver = {
	.name     = "apple_dma",
	.id_table = apple_dma_pci_ids,
	.probe    = apple_dma_probe,
	.remove   = apple_dma_remove,
};

static int __init apple_dma_init(void)
{
	int ret;

#ifndef CONFIG_ARM64
	if (window_shift) {
		pr_err("apple_dma: x86 requires exact DMA mappings; "
		       "window_shift must be 0\n");
		return -EINVAL;
	}
#endif

	pr_info("apple_dma: init (guest-side window refcount + teardown unmap)\n");

	if (window_shift > 0 && window_shift < 12) {
		pr_err("apple_dma: window_shift=%u too small (min 12)\n",
		       window_shift);
		return -EINVAL;
	}
	if (window_shift > 30) {
		pr_err("apple_dma: window_shift=%u too large (max 30)\n",
		       window_shift);
		return -EINVAL;
	}
	if (window_shift > 0) {
		wnd_size = 1ULL << window_shift;
		wnd_mask = ~(wnd_size - 1);
		pr_info("apple_dma: window coalescing enabled, "
			"window_size=%llu (%uK)\n",
			wnd_size, (unsigned)(wnd_size >> 10));
	} else {
		pr_info("apple_dma: window coalescing disabled\n");
	}

	apple_dma_trace_init();

#ifdef CONFIG_ARM64
	if (enable_tso)
		tso_patch_apply();
#endif

	ret = pci_register_driver(&apple_dma_pci_driver);
	if (ret) {
#ifdef CONFIG_ARM64
		if (enable_tso)
			tso_patch_revert();
#endif
		apple_dma_trace_exit();
	}
	return ret;
}

static void __exit apple_dma_exit(void)
{
	u32 total_maps = 0, total_windows = 0;
	u64 total_bytes = 0;

	apple_dma_trace_get_total_counts(&total_maps, &total_bytes,
					 &total_windows);

	pr_info("apple_dma: stats map=%u unmap=%u map_sg=%u unmap_sg=%u"
		" alloc=%u free=%u active=%u active_bytes=%llu"
		" windows=%u\n",
		apple_dma_trace_stat_get(APPLE_DMA_STAT_MAP),
		apple_dma_trace_stat_get(APPLE_DMA_STAT_UNMAP),
		apple_dma_trace_stat_get(APPLE_DMA_STAT_MAP_SG),
		apple_dma_trace_stat_get(APPLE_DMA_STAT_UNMAP_SG),
		apple_dma_trace_stat_get(APPLE_DMA_STAT_ALLOC),
		apple_dma_trace_stat_get(APPLE_DMA_STAT_FREE),
		total_maps, total_bytes, total_windows);
	pci_unregister_driver(&apple_dma_pci_driver);
#ifdef CONFIG_ARM64
	if (enable_tso)
		tso_patch_revert();
#endif
	apple_dma_trace_exit();
}

module_init(apple_dma_init);
module_exit(apple_dma_exit);
