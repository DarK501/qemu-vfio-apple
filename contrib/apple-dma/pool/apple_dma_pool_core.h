/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Locking-free page-range bookkeeping for the identity-mapped DMA pool.
 * Callers provide their own serialization. This header has no kernel
 * dependencies so that it can be unit tested in userspace.
 */
#ifndef APPLE_DMA_POOL_CORE_H
#define APPLE_DMA_POOL_CORE_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stddef.h>
#include <stdint.h>
typedef uint8_t u8;
typedef uint64_t u64;
#endif

#define POOL_PAGE_SHIFT 12
#define POOL_PAGE_SIZE ((size_t)1 << POOL_PAGE_SHIFT)

enum pool_kind {
	POOL_FREE = 0,
	POOL_CONT,		/* continuation page of a range */
	POOL_COHERENT,		/* first page of a coherent allocation */
	POOL_STREAM,		/* first page of a streaming bounce */
};

struct pool_slot {
	u8 kind;
	u8 dir;
	unsigned long npages;
	u64 orig_phys;		/* streaming: original buffer */
	size_t size;		/* streaming: caller-visible length */
};

struct pool_core {
	struct pool_slot *slots;	/* one per pool page */
	unsigned long npages;
};

static inline unsigned long pool_pages_for(size_t offset, size_t size)
{
	return (unsigned long)((offset + size + POOL_PAGE_SIZE - 1) >>
			       POOL_PAGE_SHIFT);
}

/*
 * First-fit search for npages free pages whose index is a multiple of
 * align_pages (a power of two). Returns the index or -1.
 */
static inline long pool_find(const struct pool_core *p, unsigned long npages,
			     unsigned long align_pages)
{
	unsigned long start, i;

	if (!npages || !align_pages || (align_pages & (align_pages - 1)) ||
	    npages > p->npages)
		return -1;

	for (start = 0; start + npages <= p->npages; start += align_pages) {
		for (i = 0; i < npages; i++)
			if (p->slots[start + i].kind != POOL_FREE)
				break;
		if (i == npages)
			return (long)start;
	}
	return -1;
}

static inline void pool_claim(struct pool_core *p, unsigned long start,
			      unsigned long npages, enum pool_kind kind)
{
	unsigned long i;

	for (i = 1; i < npages; i++)
		p->slots[start + i].kind = POOL_CONT;
	p->slots[start].kind = kind;
	p->slots[start].npages = npages;
}

/* Returns the freed page count, or 0 if start is not a range of kind. */
static inline unsigned long pool_release(struct pool_core *p,
					 unsigned long start,
					 enum pool_kind kind)
{
	unsigned long i, n;

	if (start >= p->npages || p->slots[start].kind != kind)
		return 0;
	n = p->slots[start].npages;
	for (i = 0; i < n; i++)
		p->slots[start + i].kind = POOL_FREE;
	p->slots[start].npages = 0;
	return n;
}

#endif
