/* SPDX-License-Identifier: Apache-2.0 */
/* Fixed frame pool accounting (Lichen #260). ESP-free and lock-free: the caller
 * (eh_host_frame_pool.c) holds its lock around every call. Host-tested in the
 * Lichen superproject (host_tests/frame_pool_test.c). */

#ifndef EH_FRAME_POOL_CORE_H_
#define EH_FRAME_POOL_CORE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_hosted_frame_pool.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The free set is one 64-bit mask, so a pool holds at most 64 blocks. */
#define EH_FRAME_POOL_MAX_DEPTH 64

typedef struct {
	void     *block[EH_FRAME_POOL_MAX_DEPTH];
	uint64_t  free_mask;     /* bit i set: block[i] is free */
	uint16_t  depth;
	uint16_t  block_bytes;
	uint16_t  in_use;
	bool      open;          /* takes allowed; false once closed */
	bool      live;          /* holds blocks (open, or closed with some still out) */
} eh_frame_pool_core_t;

typedef enum {
	EH_FRAME_POOL_PUT_OK,
	EH_FRAME_POOL_PUT_LAST,      /* closed and now empty: free the blocks */
	EH_FRAME_POOL_PUT_NOT_OURS,  /* not a block of this pool */
	EH_FRAME_POOL_PUT_DOUBLE,    /* a block of this pool that was already free */
} eh_frame_pool_put_t;

/* Bytes per block: `need` rounded up to `align` (a power of two). */
size_t eh_frame_pool_block_bytes(size_t need, size_t align);

/* The internal RAM a pool reserves (blocks only, not the heap's own headers). */
size_t eh_frame_pool_ram_bytes(size_t depth, size_t block_bytes);

/* Takes `depth` blocks (1..64, all non-NULL) of `block_bytes`; false if invalid. */
bool eh_frame_pool_core_init(eh_frame_pool_core_t *c, void *const *blocks,
                             uint16_t depth, uint16_t block_bytes);

/* A free block, or NULL when empty or closed. Counts in_use and peak in `s`. */
void *eh_frame_pool_core_take(eh_frame_pool_core_t *c, esp_hosted_frame_pool_stats_t *s);

/* Hands a block back. A block not handed out counts as a bad free in `s`. */
eh_frame_pool_put_t eh_frame_pool_core_put(eh_frame_pool_core_t *c, const void *p,
                                           esp_hosted_frame_pool_stats_t *s);

/* Whether `p` is one of this live pool's blocks. */
bool eh_frame_pool_core_owns(const eh_frame_pool_core_t *c, const void *p);

/* Stops takes. True when no block is out, so the caller frees the blocks now;
 * otherwise the last put returns EH_FRAME_POOL_PUT_LAST. */
bool eh_frame_pool_core_close(eh_frame_pool_core_t *c);

#ifdef __cplusplus
}
#endif

#endif /* EH_FRAME_POOL_CORE_H_ */
