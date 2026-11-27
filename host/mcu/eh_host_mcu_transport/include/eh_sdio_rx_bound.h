/*
 * SDIO RX: how big a read may be and how long it may wait for memory
 * (pure, ESP-free, host-tested; Lichen #371).
 *
 * The read length comes from the co-processor's PKT_LEN register. Trusting it
 * would let a corrupt or hostile length drain internal DMA RAM, and a size the
 * heap cannot give would park the read task in the memory wait for good. Both
 * end in a link failure instead.
 */
#ifndef EH_SDIO_RX_BOUND_H
#define EH_SDIO_RX_BOUND_H

#include <stdbool.h>
#include <stdint.h>

#include "eh_common_sdio_cfg.h"

/* Frames one stream-mode read can drain: the co-processor's in-flight depth,
 * shared with the CP. The cap when INIT advertises no aggregate, and the clamp
 * on one that is larger: a 3.0.10 C6 advertises its configured TX queue size
 * (default 20, so 30720 B) while it keeps at most 14 frames in flight, and the
 * clamp is what keeps a new P4 with that C6 at 21504 B (with a warning). */
#define EH_SDIO_RX_STREAM_MAX_FRAMES  ((uint32_t)EH_SDIO_STREAM_QUEUE_SIZE)

/* Total time a read may wait for its staging buffer before the link fails.
 * Long enough for a burst of other allocations to drain, short enough that a
 * real shortage is reported rather than hidden. */
#define EH_SDIO_RX_MEM_WAIT_MAX_MS    1500u

typedef enum {
	EH_SDIO_RX_READ,	/* buffer in hand: read it */
	EH_SDIO_RX_WAIT,	/* no buffer yet: wait and retry */
	EH_SDIO_RX_FAIL,	/* fail the link: never allocate or wait */
} eh_sdio_rx_step_t;

/* len rounded up to whole blocks; block 0 = byte-mode reads. Saturates at
 * UINT32_MAX instead of wrapping to a small value that would pass the cap. */
static inline uint32_t eh_sdio_rx_round(uint32_t len, uint32_t block)
{
	if (!block)
		return len;
	uint64_t r = ((uint64_t)len + block - 1) / block * block;
	return r > UINT32_MAX ? UINT32_MAX : (uint32_t)r;
}

/* The largest read accepted: the INIT target when there is one (already
 * rounded by the caller), never above the protocol maximum, so a hostile or
 * corrupt INIT cannot raise the cap; with no target, the maximum itself. */
static inline uint32_t eh_sdio_rx_cap(uint32_t target, uint32_t frame_bytes, uint32_t block)
{
	uint32_t max = eh_sdio_rx_round(frame_bytes * EH_SDIO_RX_STREAM_MAX_FRAMES, block);

	return (target && target < max) ? target : max;
}

/* What to do with a read of len bytes: over the cap fails at once, whatever
 * the memory; under it, a missing buffer waits until the bound runs out. */
static inline eh_sdio_rx_step_t eh_sdio_rx_step(uint32_t len, uint32_t cap, uint32_t block,
						bool have_buf, uint32_t waited_ms)
{
	if (!len || eh_sdio_rx_round(len, block) > cap)
		return EH_SDIO_RX_FAIL;
	if (have_buf)
		return EH_SDIO_RX_READ;
	return waited_ms >= EH_SDIO_RX_MEM_WAIT_MAX_MS ? EH_SDIO_RX_FAIL : EH_SDIO_RX_WAIT;
}

#endif
