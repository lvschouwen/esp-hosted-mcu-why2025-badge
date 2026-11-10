/* SPDX-License-Identifier: Apache-2.0 */
/* Fixed Wi-Fi frame pools of the SDIO host (Lichen #260): the stats a soak
 * reads to see how deep the pools get. ESP-free, so host tests include it. */

#ifndef ESP_HOSTED_FRAME_POOL_H_
#define ESP_HOSTED_FRAME_POOL_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Where a pool's blocks live. PSRAM is fine because no DMA ever touches a
 * pool block (STREAM mode copies frames in and out of internal DMA buffers). */
typedef enum {
	ESP_HOSTED_FRAME_POOL_MEM_NONE = 0,   /* no live pool */
	ESP_HOSTED_FRAME_POOL_MEM_PSRAM,
	ESP_HOSTED_FRAME_POOL_MEM_INTERNAL,   /* internal DMA RAM (PSRAM full or not allowed) */
} esp_hosted_frame_pool_mem_t;

typedef struct {
	uint16_t depth;      /* blocks in the live pool; 0 = no pool (per-frame malloc) */
	uint16_t block;      /* bytes per block */
	uint16_t in_use;     /* blocks handed out now, a retired pool's included */
	uint16_t peak;       /* in_use high-water since boot */
	uint32_t waits;      /* TX: the pool was empty and the sender waited */
	uint32_t drops;      /* TX: still empty after the wait; RX: frame dropped */
	uint32_t bad_frees;  /* a free of a block that was not handed out */
	uint32_t no_pool;    /* link-ups that could not reserve the pool */
	uint8_t  mem;        /* esp_hosted_frame_pool_mem_t of the live pool */
} esp_hosted_frame_pool_stats_t;

/* A snapshot of the TX (tx = true) or RX pool. False when pools are off. */
bool esp_hosted_frame_pool_stats(bool tx, esp_hosted_frame_pool_stats_t *out);

/* "<name> depth 10 block 1600 ram 16000 mem psram in_use 0 peak 7 waits 3 drops 0 bad_frees 0 no_pool 0";
 * returns the length written (snprintf rules). */
size_t esp_hosted_frame_pool_fmt(char *out, size_t cap, const char *name,
                                 const esp_hosted_frame_pool_stats_t *s);

#ifdef __cplusplus
}
#endif

#endif /* ESP_HOSTED_FRAME_POOL_H_ */
