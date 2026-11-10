/* SPDX-License-Identifier: Apache-2.0 */
/* Fixed Wi-Fi frame pools of the host transport (Lichen #260); see
 * eh_host_frame_pool.h. All accounting (which pool owns a block, link-down with
 * blocks out, stuck blocks) is the host-tested eh_frame_pool_core.c; this file
 * adds the memory, the lock and the TX wait. */

#include "eh_host_frame_pool.h"
#include "eh_frame_pool_core.h"

#include <string.h>

#include "esp_log.h"
#include "eh_host_port_dma.h"
#include "eh_host_port_sync.h"

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "esp_timer.h"
#else
#include <time.h>
#endif

static const char TAG[] = "eh_fpool";

enum { DIR_TX, DIR_RX, DIR_N };

static eh_frame_pool_dir_t s_dir[DIR_N];

/* Created once and never destroyed: a late put after a link-down still locks. */
static eh_host_port_mutex_t *s_lock;
static eh_host_port_sem_t   *s_tx_freed;   /* binary: a TX block came back */

/* Block lists, static rather than 256 B on each caller's stack. s_reserve is
 * only used by create (the bus init, one caller); s_to_free only under s_lock. */
static void *s_reserve[EH_FRAME_POOL_MAX_DEPTH];
static void *s_to_free[EH_FRAME_POOL_MAX_DEPTH];

static const char *dir_name(int d) { return d == DIR_TX ? "tx" : "rx"; }

static uint64_t now_ms(void)
{
#ifdef ESP_PLATFORM
	return (uint64_t)(esp_timer_get_time() / 1000);
#else
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
#endif
}

static void *block_alloc(size_t bytes, uint8_t mem)
{
#ifdef ESP_PLATFORM
	if (mem == ESP_HOSTED_FRAME_POOL_MEM_PSRAM)
		return heap_caps_aligned_alloc(HOSTED_MEM_ALIGNMENT, bytes,
		                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
	/* Explicitly internal: eh_host_port_dma_alloc_aligned may prefer PSRAM. */
	return heap_caps_aligned_alloc(HOSTED_MEM_ALIGNMENT, bytes,
	                               MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
#else
	(void)mem;
	return eh_host_port_dma_alloc_aligned(bytes, HOSTED_MEM_ALIGNMENT);
#endif
}

static void blocks_free(void *const *blocks, uint16_t n)
{
	for (uint16_t i = 0; i < n; i++)
		eh_host_port_dma_free(blocks[i]);
}

void *eh_host_frame_tx_heap_alloc(size_t n)
{
#ifdef ESP_PLATFORM
	if (s_dir[DIR_TX].psram_ok) {
		void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
		if (p)
			return p;
	}
#endif
	return eh_host_port_dma_alloc(n);
}

static bool ensure_sync(void)
{
	if (!s_lock)
		s_lock = eh_host_port_mutex_create();
	if (!s_tx_freed) {
		s_tx_freed = eh_host_port_sem_create();
		if (s_tx_freed)
			eh_host_port_sem_try_wait(s_tx_freed);
	}
	return s_lock && s_tx_freed;
}

/* Reserves all blocks in one kind of memory or none; called with no lock. */
static bool reserve_in(uint16_t depth, uint16_t block, uint8_t mem)
{
	for (uint16_t i = 0; i < depth; i++) {
		s_reserve[i] = block_alloc(block, mem);
		if (!s_reserve[i]) {
			blocks_free(s_reserve, i);
			return false;
		}
	}
	return true;
}

/* PSRAM first when allowed, then internal DMA. Returns the memory kind, NONE on failure. */
static uint8_t reserve(uint16_t depth, uint16_t block, bool psram_ok)
{
	if (!depth || depth > EH_FRAME_POOL_MAX_DEPTH)
		return ESP_HOSTED_FRAME_POOL_MEM_NONE;
	if (psram_ok && reserve_in(depth, block, ESP_HOSTED_FRAME_POOL_MEM_PSRAM))
		return ESP_HOSTED_FRAME_POOL_MEM_PSRAM;
	if (reserve_in(depth, block, ESP_HOSTED_FRAME_POOL_MEM_INTERNAL))
		return ESP_HOSTED_FRAME_POOL_MEM_INTERNAL;
	return ESP_HOSTED_FRAME_POOL_MEM_NONE;
}

static void dir_create(int d, uint16_t depth, uint16_t block, bool psram_ok)
{
	eh_frame_pool_dir_t *f = &s_dir[d];

	f->psram_ok = psram_ok;
	uint8_t mem = reserve(depth, block, psram_ok);
	eh_host_port_mutex_lock(s_lock);
	/* Refuses a second link-up without a link-down (the live pool stays). */
	bool ok = mem != ESP_HOSTED_FRAME_POOL_MEM_NONE &&
	          eh_frame_pool_dir_install(f, s_reserve, depth, block, mem);
	if (!ok)
		f->st.no_pool++;
	eh_host_port_mutex_unlock(s_lock);

	if (!ok) {
		ESP_LOGE(TAG, "%s pool %u x %u B not installed (%s): per-frame heap this link-up",
		         dir_name(d), (unsigned)depth, (unsigned)block,
		         mem == ESP_HOSTED_FRAME_POOL_MEM_NONE ? "no memory" : "a pool is still live");
		if (mem != ESP_HOSTED_FRAME_POOL_MEM_NONE)
			blocks_free(s_reserve, depth);
		return;
	}
	ESP_LOGI(TAG, "%s pool: %u x %u B = %u B %s", dir_name(d), (unsigned)depth,
	         (unsigned)block, (unsigned)eh_frame_pool_ram_bytes(depth, block),
	         mem == ESP_HOSTED_FRAME_POOL_MEM_PSRAM ? "PSRAM" :
	         psram_ok ? "internal DMA (PSRAM full)" : "internal DMA");
}

void eh_host_frame_pools_create(uint16_t tx_depth, uint16_t tx_block, bool tx_psram,
                                uint16_t rx_depth, uint16_t rx_block, bool rx_psram)
{
	if (!ensure_sync()) {
		ESP_LOGE(TAG, "no lock for the frame pools: per-frame heap");
		return;
	}
	dir_create(DIR_TX, tx_depth, tx_block, tx_psram);
	dir_create(DIR_RX, rx_depth, rx_block, rx_psram);
}

static void dir_destroy(int d)
{
	eh_frame_pool_dir_t *f = &s_dir[d];

	eh_host_port_mutex_lock(s_lock);
	uint32_t lost_before = f->st.lost;
	uint16_t n = eh_frame_pool_dir_retire(f, s_to_free);
	blocks_free(s_to_free, n);   /* under the lock: s_to_free is shared */
	uint32_t lost = f->st.lost - lost_before;
	uint16_t out  = f->old.live ? f->old.in_use : 0;
	eh_host_port_mutex_unlock(s_lock);

	if (out)
		ESP_LOGW(TAG, "%s pool: %u block(s) still out at link-down; freed when back",
		         dir_name(d), (unsigned)out);
	if (lost)
		ESP_LOGE(TAG, "%s pool: %lu block(s) of the link before never came back: left allocated",
		         dir_name(d), (unsigned long)lost);
}

void eh_host_frame_pools_destroy(void)
{
	if (!s_lock)
		return;
	dir_destroy(DIR_TX);
	dir_destroy(DIR_RX);
	/* A TX sender waiting on an empty pool gives up now instead of at its timeout. */
	eh_host_port_sem_post(s_tx_freed);
}

void *eh_host_frame_pool_rx_get(size_t n, eh_fp_get_t *why)
{
	eh_fp_get_t w = EH_FP_OFF;
	void *p = NULL;
	if (s_lock) {
		eh_frame_pool_dir_t *f = &s_dir[DIR_RX];
		eh_host_port_mutex_lock(s_lock);
		p = eh_frame_pool_dir_take(f, n, &w);
		if (w == EH_FP_EMPTY)
			f->st.drops++;
		eh_host_port_mutex_unlock(s_lock);
	}
	if (why)
		*why = w;
	return p;
}

/* Under the lock, with the pool empty: waits until a block comes back or the
 * deadline passes. A put wakes it at once; a stale post only costs a retry. */
static void *tx_wait(eh_frame_pool_dir_t *f, size_t n, uint32_t wait_ms, eh_fp_get_t *w)
{
	void *p = NULL;
	uint64_t deadline = now_ms() + wait_ms;
	f->st.waits++;
	f->waiters++;
	for (uint64_t t = now_ms(); *w == EH_FP_EMPTY && t < deadline; t = now_ms()) {
		eh_host_port_mutex_unlock(s_lock);
		eh_host_port_sem_wait_ms(s_tx_freed, (uint32_t)(deadline - t));
		eh_host_port_mutex_lock(s_lock);
		p = eh_frame_pool_dir_take(f, n, w);
	}
	f->waiters--;
	return p;
}

void *eh_host_frame_pool_tx_get(size_t n, uint32_t wait_ms, eh_fp_get_t *why)
{
	eh_fp_get_t w = EH_FP_OFF;
	void *p = NULL;
	eh_frame_pool_dir_t *f = &s_dir[DIR_TX];

	if (s_lock) {
		eh_host_port_mutex_lock(s_lock);
		p = eh_frame_pool_dir_take(f, n, &w);
		if (w == EH_FP_EMPTY && wait_ms)
			p = tx_wait(f, n, wait_ms, &w);
		if (w == EH_FP_EMPTY)
			f->st.drops++;
		eh_host_port_mutex_unlock(s_lock);
	}
	if (why)
		*why = w;
	return p;
}

static void dir_put(int d, void *p)
{
	eh_frame_pool_dir_t *f = &s_dir[d];

	if (!p || !s_lock)
		return;
	eh_host_port_mutex_lock(s_lock);
	uint32_t bad  = f->st.bad_frees;
	bool     free_now = eh_frame_pool_dir_put(f, p);
	bool     was_bad  = f->st.bad_frees != bad;
	bool     wake     = f->waiters > 0;
	eh_host_port_mutex_unlock(s_lock);

	if (was_bad)
		ESP_LOGE(TAG, "%s pool: bad free of %p (already free or not a pool block)",
		         dir_name(d), p);
	if (wake && d == DIR_TX)
		eh_host_port_sem_post(s_tx_freed);
	if (free_now)
		eh_host_port_dma_free(p);
}

void eh_host_frame_pool_tx_put(void *p) { dir_put(DIR_TX, p); }
void eh_host_frame_pool_rx_put(void *p) { dir_put(DIR_RX, p); }

bool esp_hosted_frame_pool_stats(bool tx, esp_hosted_frame_pool_stats_t *out)
{
	if (!out)
		return false;
	memset(out, 0, sizeof(*out));
	if (!s_lock)
		return false;
	eh_host_port_mutex_lock(s_lock);
	*out = s_dir[tx ? DIR_TX : DIR_RX].st;
	eh_host_port_mutex_unlock(s_lock);
	return true;
}
