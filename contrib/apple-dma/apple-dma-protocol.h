/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The mapping token identifies exactly one registered memory range and is
 * not a DMA address. Segments are ordered by offset and must cover [0, size)
 * exactly, without gaps or overlap. Their addresses are the only addresses
 * guest devices may use for DMA. A successful map has status OK and at least
 * one segment. ERROR means this request did not create a mapping, even if a
 * different mapping already uses the same token. RETAINED means this request
 * may have created a mapping; its token must be unmapped before its guest
 * memory is released. If ownership is uncertain, unmap fails rather than
 * removing a potentially unrelated mapping; the guest must not release the
 * backing. The token can be zero. A mapping remains live until a successful
 * unmap response.
 */

#ifndef APPLE_DMA_PROTOCOL_H
#define APPLE_DMA_PROTOCOL_H

#define APPLE_DMA_PROTOCOL_VERSION 3
#define APPLE_DMA_MAX_SEGMENTS 32
#define APPLE_DMA_MAP_STATUS_OK 0
#define APPLE_DMA_MAP_STATUS_ERROR 1
#define APPLE_DMA_MAP_STATUS_RETAINED 2

/* Optional version-3 extension, advertised at BAR0 offset 0x1c. */
#define APPLE_DMA_FEATURE_MAPPING_ID 1U
#define APPLE_DMA_MAP_FLAG_MAPPING_ID 0x80000000U
#define APPLE_DMA_MAP_ID_MASK 0x7fffffffU
#define APPLE_DMA_MAP_ID_PREFIX 0x8000000000000000ULL

#ifdef __KERNEL__
#include <linux/types.h>
typedef __le64 apple_dma_le64_t;
typedef __le32 apple_dma_le32_t;
#else
#include <stdint.h>
typedef uint64_t apple_dma_le64_t;
typedef uint32_t apple_dma_le32_t;
#endif

typedef struct AppleDMASegment {
    apple_dma_le64_t address;
    apple_dma_le64_t length;
    apple_dma_le64_t offset;
} AppleDMASegment;

typedef struct AppleDMAMapResult {
    apple_dma_le64_t mapping_id;
    apple_dma_le32_t segment_count;
    apple_dma_le32_t status;
    AppleDMASegment segments[APPLE_DMA_MAX_SEGMENTS];
} AppleDMAMapResult;

#endif
