/* SPDX-License-Identifier: Apache-2.0 */
/* Fixed Wi-Fi frame pools of the host transport (Lichen #260); see
 * eh_host_frame_pool.h. The accounting is eh_frame_pool_core.c; this file adds
 * the memory, the lock and the TX wait. */

#include "eh_host_frame_pool.h"
#include "eh_frame_pool_core.h"

#include <string.h>

#include "esp_log.h"
#include "eh_host_port_dma.h"
#include "eh_host_port_sync.h"

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

static const char TAG[] = "eh_fpool";

enum { DIR_TX, DIR_RX, DIR_N };

/* Two cores per direction: the live pool, and a retired one that still has
 * blocks out after a link-down while the next link-up already runs. */
typedef struct {
	eh_frame_pool_core_t          core[2];
	int                           cur;      /* index of the live core, -1 = none */
	esp_hosted_frame_pool_stats_t st;       /* since boot, across link-ups */
	uint16_t                      waiters;
	bool                          psram_ok; /* the bus copies these frames (no DMA) */
} fp_dir_t;

static fp_dir_t s_dir[DIR_N] = {{.cur = -1}, {.cur = -1}};

/* Created once and never destroyed: a late put after a link-down still locks. */
static eh_host_port_mutex_t *s_lock;
static eh_host_port_sem_t   *s_tx_freed;   /* binary: a TX block came back */

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

static void blocks_free(void *const *blocks, uint16_t n)
{
	for (uint16_t i = 0; i < n; i++)
		eh_host_port_dma_free(blocks[i]);
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
static bool reserve_in(void **blocks, uint16_t depth, uint16_t block, uint8_t mem)
{
	for (uint16_t i = 0; i < depth; i++) {
		blocks[i] = block_alloc(block, mem);
		if (!blocks[i]) {
			blocks_free(blocks, i);
			return false;
		}
	}
	return true;
}

/* PSRAM first when allowed, then internal DMA. Returns the memory kind, NONE on failure. */
static uint8_t reserve(void **blocks, uint16_t depth, uint16_t block, bool psram_ok)
{
	if (psram_ok && reserve_in(blocks, depth, block, ESP_HOSTED_FRAME_POOL_MEM_PSRAM))
		return ESP_HOSTED_FRAME_POOL_MEM_PSRAM;
	if (reserve_in(blocks, depth, block, ESP_HOSTED_FRAME_POOL_MEM_INTERNAL))
		return ESP_HOSTED_FRAME_POOL_MEM_INTERNAL;
	return ESP_HOSTED_FRAME_POOL_MEM_NONE;
}

static void dir_create(int d, uint16_t depth, uint16_t block, bool psram_ok)
{
	void *blocks[EH_FRAME_POOL_MAX_DEPTH] = {0};
	fp_dir_t *f = &s_dir[d];
	const char *name = d == DIR_TX ? "tx" : "rx";
	uint8_t mem = ESP_HOSTED_FRAME_POOL_MEM_NONE;

	f->psram_ok = psram_ok;
	if (depth && depth <= EH_FRAME_POOL_MAX_DEPTH)
		mem = reserve(blocks, depth, block, psram_ok);
	if (mem == ESP_HOSTED_FRAME_POOL_MEM_NONE) {
		ESP_LOGE(TAG, "%s pool %u x %u B not reserved: per-frame heap this link-up",
		         name, (unsigned)depth, (unsigned)block);
		eh_host_port_mutex_lock(s_lock);
		f->st.no_pool++;
		eh_host_port_mutex_unlock(s_lock);
		return;
	}
	eh_host_port_mutex_lock(s_lock);
	int slot = f->core[0].live ? (f->core[1].live ? -1 : 1) : 0;
	bool ok  = slot >= 0 && eh_frame_pool_core_init(&f->core[slot], blocks, depth, block);
	if (ok) {
		f->cur      = slot;
		f->st.depth = depth;
		f->st.block = block;
		f->st.mem   = mem;
	} else {
		f->st.no_pool++;
	}
	eh_host_port_mutex_unlock(s_lock);
	if (!ok) {
		/* Both cores still hold blocks from earlier link-ups. */
		ESP_LOGE(TAG, "%s pool: two retired pools still in use: per-frame heap", name);
		blocks_free(blocks, depth);
		return;
	}
	ESP_LOGI(TAG, "%s pool: %u x %u B = %u B %s", name, (unsigned)depth,
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
	void *blocks[EH_FRAME_POOL_MAX_DEPTH];
	uint16_t n = 0;
	fp_dir_t *f = &s_dir[d];

	eh_host_port_mutex_lock(s_lock);
	if (f->cur >= 0) {
		eh_frame_pool_core_t *c = &f->core[f->cur];
		if (eh_frame_pool_core_close(c)) {
			n = c->depth;
			memcpy(blocks, c->block, n * sizeof(blocks[0]));
		} else {
			ESP_LOGW(TAG, "%s pool: %u block(s) still out at link-down; freed when back",
			         d == DIR_TX ? "tx" : "rx", (unsigned)c->in_use);
		}
		f->cur      = -1;
		f->st.depth = 0;
		f->st.mem   = ESP_HOSTED_FRAME_POOL_MEM_NONE;
	}
	eh_host_port_mutex_unlock(s_lock);
	blocks_free(blocks, n);
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

/* Under the lock: a block of the live pool, or why not. */
static void *dir_take(fp_dir_t *f, size_t n, eh_fp_get_t *why)
{
	if (f->cur < 0) {
		*why = EH_FP_OFF;
		return NULL;
	}
	if (n > f->core[f->cur].block_bytes) {
		*why = EH_FP_TOO_BIG;
		return NULL;
	}
	void *p = eh_frame_pool_core_take(&f->core[f->cur], &f->st);
	*why = p ? EH_FP_GOT : EH_FP_EMPTY;
	return p;
}

void *eh_host_frame_pool_rx_get(size_t n, eh_fp_get_t *why)
{
	eh_fp_get_t w = EH_FP_OFF;
	void *p = NULL;
	if (s_lock) {
		fp_dir_t *f = &s_dir[DIR_RX];
		eh_host_port_mutex_lock(s_lock);
		p = dir_take(f, n, &w);
		if (w == EH_FP_EMPTY)
			f->st.drops++;
		eh_host_port_mutex_unlock(s_lock);
	}
	if (why)
		*why = w;
	return p;
}

void *eh_host_frame_pool_tx_get(size_t n, uint32_t wait_ms, eh_fp_get_t *why)
{
	eh_fp_get_t w = EH_FP_OFF;
	void *p = NULL;
	fp_dir_t *f = &s_dir[DIR_TX];

	if (s_lock) {
		eh_host_port_mutex_lock(s_lock);
		p = dir_take(f, n, &w);
		if (w == EH_FP_EMPTY && wait_ms) {
			f->st.waits++;
			f->waiters++;
			/* 1 ms slices: a put wakes us at once, and the total stays
			 * bounded by wait_ms even when stale posts end slices early. */
			for (uint32_t ms = 0; ms < wait_ms && w == EH_FP_EMPTY; ms++) {
				eh_host_port_mutex_unlock(s_lock);
				eh_host_port_sem_wait_ms(s_tx_freed, 1);
				eh_host_port_mutex_lock(s_lock);
				p = dir_take(f, n, &w);
			}
			f->waiters--;
		}
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
	void *blocks[EH_FRAME_POOL_MAX_DEPTH];
	uint16_t n = 0;
	fp_dir_t *f = &s_dir[d];
	eh_frame_pool_put_t r = EH_FRAME_POOL_PUT_NOT_OURS;
	bool wake;

	if (!p || !s_lock)
		return;
	eh_host_port_mutex_lock(s_lock);
	for (int i = 0; i < 2 && r == EH_FRAME_POOL_PUT_NOT_OURS; i++) {
		r = eh_frame_pool_core_put(&f->core[i], p, &f->st);
		if (r == EH_FRAME_POOL_PUT_LAST) {
			n = f->core[i].depth;
			memcpy(blocks, f->core[i].block, n * sizeof(blocks[0]));
		}
	}
	if (r == EH_FRAME_POOL_PUT_NOT_OURS)
		f->st.bad_frees++;
	wake = f->waiters > 0;
	eh_host_port_mutex_unlock(s_lock);

	if (r == EH_FRAME_POOL_PUT_NOT_OURS || r == EH_FRAME_POOL_PUT_DOUBLE)
		ESP_LOGE(TAG, "%s pool: bad free of %p (%s)", d == DIR_TX ? "tx" : "rx", p,
		         r == EH_FRAME_POOL_PUT_DOUBLE ? "already free" : "not a pool block");
	if (wake && d == DIR_TX)
		eh_host_port_sem_post(s_tx_freed);
	blocks_free(blocks, n);
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
