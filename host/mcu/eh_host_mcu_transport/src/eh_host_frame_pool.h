/* SPDX-License-Identifier: Apache-2.0 */
/* Fixed Wi-Fi frame pools of the host transport (Lichen #260).
 *
 * Why: a DMA malloc per frame let the internal DMA heap's peak follow the
 * traffic and mixed short-lived frames into a ~100 KB heap of long-lived
 * blocks. The pools reserve their blocks once at link-up and free them at
 * link-down.
 *
 * Memory: PSRAM first when the caller allows it, else internal DMA RAM. A pool
 * block is never a DMA source or target: in SDIO STREAM mode TX frames are
 * copied into the internal write buffer and RX sub-frames are copied out of the
 * internal staging buffer. The bus keeps it that way (eh_host_bus_sdio.c guards),
 * because SDMMC DMA into or out of PSRAM flips bits at 120 MHz (Lichen #121).
 * Since only the CPU touches a block, its memcpy from or to PSRAM goes through the
 * cache coherently and needs no esp_cache_msync.
 *
 * Only Wi-Fi data frames (STA/AP) use them; control frames (RPC, HCI) stay on
 * the heap so Wi-Fi traffic can never starve the RPC link.
 * All calls are task context (not ISR). */

#ifndef EH_HOST_FRAME_POOL_H_
#define EH_HOST_FRAME_POOL_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_hosted_frame_pool.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
	EH_FP_GOT,      /* a pool block */
	EH_FP_EMPTY,    /* the pool is empty (TX: after the wait); counted as a drop */
	EH_FP_OFF,      /* no pool this link-up: use the per-frame heap path */
	EH_FP_TOO_BIG,  /* larger than a block: use the per-frame heap path */
} eh_fp_get_t;

/* At link-up. *_psram: the bus copies this direction's frames, so its blocks
 * may live in PSRAM (tried first, then internal DMA). A pool whose blocks
 * cannot all be reserved stays off (counted in no_pool) and its direction
 * falls back to the per-frame heap. */
void eh_host_frame_pools_create(uint16_t tx_depth, uint16_t tx_block, bool tx_psram,
                                uint16_t rx_depth, uint16_t rx_block, bool rx_psram);

/* At link-down, after the queues are drained. Blocks still out (a sender
 * between its get and the queue) go back to the retired pool, and its
 * memory is freed with the last one. */
void eh_host_frame_pools_destroy(void);

/* TX waits up to wait_ms for a block (backpressure), RX never waits. */
void *eh_host_frame_pool_tx_get(size_t n, uint32_t wait_ms, eh_fp_get_t *why);
void *eh_host_frame_pool_rx_get(size_t n, eh_fp_get_t *why);

/* The per-frame heap path (no pool, too big, or a control frame): PSRAM first
 * when the last create allowed PSRAM for TX, else internal DMA. Free with
 * eh_host_port_dma_free. */
void *eh_host_frame_tx_heap_alloc(size_t n);

/* free_buf_handle of a pooled frame. */
void eh_host_frame_pool_tx_put(void *p);
void eh_host_frame_pool_rx_put(void *p);

#ifdef __cplusplus
}
#endif

#endif /* EH_HOST_FRAME_POOL_H_ */
