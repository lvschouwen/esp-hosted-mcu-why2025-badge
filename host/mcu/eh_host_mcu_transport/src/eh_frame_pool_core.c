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
	if (c->free_mask & bit) {
		if (s)
			s->bad_frees++;
		return EH_FRAME_POOL_PUT_DOUBLE;
	}
	c->free_mask |= bit;
	c->in_use--;
	if (s && s->in_use)
		s->in_use--;
	if (!c->open && !c->in_use) {
		c->live = false;
		return EH_FRAME_POOL_PUT_LAST;
	}
	return EH_FRAME_POOL_PUT_OK;
}

bool eh_frame_pool_core_close(eh_frame_pool_core_t *c)
{
	if (!c || !c->live)
		return false;
	c->open = false;
	if (c->in_use)
		return false;
	c->live = false;
	return true;
}

size_t esp_hosted_frame_pool_fmt(char *out, size_t cap, const char *name,
                                 const esp_hosted_frame_pool_stats_t *s)
{
	if (!out || !cap || !s)
		return 0;
	int n = snprintf(out, cap,
		"%s depth %u block %u ram %u in_use %u peak %u waits %lu drops %lu bad_frees %lu no_pool %lu",
		name ? name : "pool", (unsigned)s->depth, (unsigned)s->block,
		(unsigned)eh_frame_pool_ram_bytes(s->depth, s->block), (unsigned)s->in_use,
		(unsigned)s->peak, (unsigned long)s->waits, (unsigned long)s->drops,
		(unsigned long)s->bad_frees, (unsigned long)s->no_pool);
	return n < 0 ? 0 : (size_t)n;
}
