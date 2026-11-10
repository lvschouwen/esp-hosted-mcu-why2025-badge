/* SPDX-License-Identifier: Apache-2.0 */
/* Fixed frame pool accounting (Lichen #260). ESP-free and lock-free: the caller
 * (eh_host_frame_pool.c) holds its lock around every call and does the actual
 * allocating and freeing of the blocks this file names. Host-tested in the
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

/* One pool: `depth` blocks. Once closed, a block is never handed out again,
 * and each block that comes back is freed at once (its slot goes NULL). */
typedef struct {
	void     *block[EH_FRAME_POOL_MAX_DEPTH];
	uint64_t  free_mask;     /* bit i set: block[i] is free (open pools only) */
	uint16_t  depth;
	uint16_t  block_bytes;
	uint16_t  in_use;
	bool      open;          /* takes allowed; false once closed */
	bool      live;          /* holds blocks (open, or closed with some still out) */
} eh_frame_pool_core_t;

typedef enum {
	EH_FRAME_POOL_PUT_OK,        /* back in the free set */
	EH_FRAME_POOL_PUT_FREE,      /* its pool is closed: the caller frees the block now */
	EH_FRAME_POOL_PUT_NOT_OURS,  /* not a block of this pool */
	EH_FRAME_POOL_PUT_DOUBLE,    /* a block of this pool that was already free */
} eh_frame_pool_put_t;

typedef enum {
	EH_FP_GOT,      /* a pool block */
	EH_FP_EMPTY,    /* the pool is empty */
	EH_FP_OFF,      /* no pool this link-up: use the per-frame heap path */
	EH_FP_TOO_BIG,  /* larger than a block: use the per-frame heap path */
} eh_fp_get_t;

/* Bytes per block: `need` rounded up to `align` (a power of two). */
size_t eh_frame_pool_block_bytes(size_t need, size_t align);

/* The RAM a pool reserves (blocks only, not the heap's own headers). */
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

/* Stops takes and moves every free block to `to_free` (room for 64) for the
 * caller to free now; returns how many. Blocks still out stay with the pool
 * and are freed as they come back (EH_FRAME_POOL_PUT_FREE). */
uint16_t eh_frame_pool_core_close(eh_frame_pool_core_t *c, void **to_free);

/* Gives up on a closed pool's blocks still out and forgets them (never freed:
 * see eh_frame_pool_dir_retire). Returns how many. */
uint16_t eh_frame_pool_core_abandon(eh_frame_pool_core_t *c);

/* One direction (TX or RX): the live pool, and the pool of the previous link
 * that still waits for blocks out at its link-down. */
typedef struct {
	eh_frame_pool_core_t          cur;
	eh_frame_pool_core_t          old;
	esp_hosted_frame_pool_stats_t st;        /* since boot, across link-ups */
	uint16_t                      waiters;   /* TX senders waiting for a block */
	bool                          psram_ok;  /* the bus copies these frames (no DMA) */
} eh_frame_pool_dir_t;

/* At link-up: `blocks` become the live pool, in memory `mem`
 * (esp_hosted_frame_pool_mem_t). False, and nothing taken, when a live pool is
 * still open (link-up without link-down) or the input is invalid. */
bool eh_frame_pool_dir_install(eh_frame_pool_dir_t *d, void *const *blocks, uint16_t depth,
                               uint16_t block_bytes, uint8_t mem);

/* A block of the live pool for `n` bytes, or NULL and why. Counts no drop. */
void *eh_frame_pool_dir_take(eh_frame_pool_dir_t *d, size_t n, eh_fp_get_t *why);

/* Hands a block back to whichever pool owns it. True: the caller frees `p` now
 * (its pool is closed). A pointer neither pool owns counts as a bad free. */
bool eh_frame_pool_dir_put(eh_frame_pool_dir_t *d, const void *p);

/* At link-down, after the queues are drained and the bus threads joined:
 * closes the live pool and puts its free blocks in `to_free` (room for 64)
 * for the caller to free; returns how many. The live pool becomes the old one,
 * so blocks still out return to it through the next link session. An old pool
 * still holding blocks by now (a whole link session later) is abandoned: its
 * blocks are counted in `lost` and never freed, because a block that never came
 * back may still be written by its holder, and its address could come back as a
 * block of a new pool, which a stale put would then free while in use. That
 * costs the stuck blocks only (not the pool), and the two cores never run out. */
uint16_t eh_frame_pool_dir_retire(eh_frame_pool_dir_t *d, void **to_free);

#ifdef __cplusplus
}
#endif

#endif /* EH_FRAME_POOL_CORE_H_ */
