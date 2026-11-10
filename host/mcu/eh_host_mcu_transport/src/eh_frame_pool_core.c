/* SPDX-License-Identifier: Apache-2.0 */
/* Fixed frame pool accounting (Lichen #260); see eh_frame_pool_core.h. */

#include "eh_frame_pool_core.h"

#include <stdio.h>
#include <string.h>

size_t eh_frame_pool_block_bytes(size_t need, size_t align)
{
	if (!align || (align & (align - 1u)))
		return 0;
	return (need + align - 1u) & ~(align - 1u);
}

size_t eh_frame_pool_ram_bytes(size_t depth, size_t block_bytes)
{
	return depth * block_bytes;
}

bool eh_frame_pool_core_init(eh_frame_pool_core_t *c, void *const *blocks,
                             uint16_t depth, uint16_t block_bytes)
{
	if (!c || !blocks || !depth || depth > EH_FRAME_POOL_MAX_DEPTH || !block_bytes)
		return false;
	for (uint16_t i = 0; i < depth; i++) {
		if (!blocks[i])
			return false;
	}
	memset(c, 0, sizeof(*c));
	memcpy(c->block, blocks, depth * sizeof(blocks[0]));
	c->depth       = depth;
	c->block_bytes = block_bytes;
	c->free_mask   = depth == 64 ? UINT64_MAX : ((uint64_t)1 << depth) - 1u;
	c->open        = true;
	c->live        = true;
	return true;
}

void *eh_frame_pool_core_take(eh_frame_pool_core_t *c, esp_hosted_frame_pool_stats_t *s)
{
	if (!c || !c->open || !c->free_mask)
		return NULL;
	int i = __builtin_ctzll(c->free_mask);
	c->free_mask &= ~((uint64_t)1 << i);
	c->in_use++;
	if (s) {
		s->in_use++;
		if (s->in_use > s->peak)
			s->peak = s->in_use;
	}
	return c->block[i];
}

static int core_index(const eh_frame_pool_core_t *c, const void *p)
{
	if (!c->live || !p)
		return -1;
	for (int i = 0; i < c->depth; i++) {
		if (c->block[i] == p)
			return i;
	}
	return -1;
}

bool eh_frame_pool_core_owns(const eh_frame_pool_core_t *c, const void *p)
{
	return c && core_index(c, p) >= 0;
}

eh_frame_pool_put_t eh_frame_pool_core_put(eh_frame_pool_core_t *c, const void *p,
                                           esp_hosted_frame_pool_stats_t *s)
{
	int i = c ? core_index(c, p) : -1;
	if (i < 0)
		return EH_FRAME_POOL_PUT_NOT_OURS;
	uint64_t bit = (uint64_t)1 << i;
	if (c->open && (c->free_mask & bit)) {
		if (s)
			s->bad_frees++;
		return EH_FRAME_POOL_PUT_DOUBLE;
	}
	c->in_use--;
	if (s && s->in_use)
		s->in_use--;
	if (c->open) {
		c->free_mask |= bit;
		return EH_FRAME_POOL_PUT_OK;
	}
	/* Closed: the block leaves the pool, so a second put of it is not ours. */
	c->block[i] = NULL;
	if (!c->in_use)
		c->live = false;
	return EH_FRAME_POOL_PUT_FREE;
}

uint16_t eh_frame_pool_core_close(eh_frame_pool_core_t *c, void **to_free)
{
	uint16_t n = 0;
	if (!c || !c->live)
		return 0;
	c->open = false;
	for (int i = 0; i < c->depth; i++) {
		if (c->free_mask & ((uint64_t)1 << i)) {
			to_free[n++] = c->block[i];
			c->block[i] = NULL;
		}
	}
	c->free_mask = 0;
	if (!c->in_use)
		c->live = false;
	return n;
}

uint16_t eh_frame_pool_core_abandon(eh_frame_pool_core_t *c)
{
	if (!c || !c->live)
		return 0;
	uint16_t n = c->in_use;
	memset(c, 0, sizeof(*c));
	return n;
}

bool eh_frame_pool_dir_install(eh_frame_pool_dir_t *d, void *const *blocks, uint16_t depth,
                               uint16_t block_bytes, uint8_t mem)
{
	if (!d || d->cur.open)
		return false;
	if (!eh_frame_pool_core_init(&d->cur, blocks, depth, block_bytes))
		return false;
	d->st.depth = depth;
	d->st.block = block_bytes;
	d->st.mem   = mem;
	return true;
}

void *eh_frame_pool_dir_take(eh_frame_pool_dir_t *d, size_t n, eh_fp_get_t *why)
{
	if (!d->cur.open) {
		*why = EH_FP_OFF;
		return NULL;
	}
	if (n > d->cur.block_bytes) {
		*why = EH_FP_TOO_BIG;
		return NULL;
	}
	void *p = eh_frame_pool_core_take(&d->cur, &d->st);
	*why = p ? EH_FP_GOT : EH_FP_EMPTY;
	return p;
}

bool eh_frame_pool_dir_put(eh_frame_pool_dir_t *d, const void *p)
{
	if (!p)
		return false;   /* like free(NULL) */
	eh_frame_pool_put_t r = eh_frame_pool_core_put(&d->cur, p, &d->st);
	if (r == EH_FRAME_POOL_PUT_NOT_OURS)
		r = eh_frame_pool_core_put(&d->old, p, &d->st);
	if (r == EH_FRAME_POOL_PUT_NOT_OURS)
		d->st.bad_frees++;
	return r == EH_FRAME_POOL_PUT_FREE;
}

uint16_t eh_frame_pool_dir_retire(eh_frame_pool_dir_t *d, void **to_free)
{
	uint16_t lost = eh_frame_pool_core_abandon(&d->old);
	d->st.lost   += lost;
	d->st.in_use  = d->st.in_use > lost ? (uint16_t)(d->st.in_use - lost) : 0;

	uint16_t n = eh_frame_pool_core_close(&d->cur, to_free);
	if (d->cur.live)
		d->old = d->cur;   /* blocks still out: they return to it */
	memset(&d->cur, 0, sizeof(d->cur));
	d->st.depth = 0;
	d->st.mem   = ESP_HOSTED_FRAME_POOL_MEM_NONE;
	return n;
}

static const char *mem_name(uint8_t mem)
{
	switch (mem) {
	case ESP_HOSTED_FRAME_POOL_MEM_PSRAM:    return "psram";
	case ESP_HOSTED_FRAME_POOL_MEM_INTERNAL: return "internal";
	default:                                 return "none";
	}
}

size_t esp_hosted_frame_pool_fmt(char *out, size_t cap, const char *name,
                                 const esp_hosted_frame_pool_stats_t *s)
{
	if (!out || !cap || !s)
		return 0;
	int n = snprintf(out, cap,
		"%s depth %u block %u ram %u mem %s in_use %u peak %u waits %lu drops %lu bad_frees %lu "
		"lost %lu no_pool %lu",
		name ? name : "pool", (unsigned)s->depth, (unsigned)s->block,
		(unsigned)eh_frame_pool_ram_bytes(s->depth, s->block), mem_name(s->mem), (unsigned)s->in_use,
		(unsigned)s->peak, (unsigned long)s->waits, (unsigned long)s->drops,
		(unsigned long)s->bad_frees, (unsigned long)s->lost, (unsigned long)s->no_pool);
	return n < 0 ? 0 : (size_t)n;
}
