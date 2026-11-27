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

/* Frames one stream-mode read can drain: the co-processor's in-flight depth
 * (SDIO_STREAM_QUEUE_SIZE, 14 in 3.0.10 and in main). The cap when INIT
 * advertised no aggregate (an older co-processor sends no TX_Q_SIZE). */
#define EH_SDIO_RX_STREAM_MAX_FRAMES  14u

/* Total time a read may wait for its staging buffer before the link fails.
 * Long enough for a burst of other allocations to drain, short enough that a
 * real shortage is reported rather than hidden. */
#define EH_SDIO_RX_MEM_WAIT_MAX_MS    1500u

typedef enum {
	EH_SDIO_RX_READ,	/* buffer in hand: read it */
	EH_SDIO_RX_WAIT,	/* no buffer yet: wait and retry */
	EH_SDIO_RX_FAIL,	/* fail the link: never allocate or wait */
} eh_sdio_rx_step_t;

/* len rounded up to whole blocks; block 0 = byte-mode reads. */
static inline uint32_t eh_sdio_rx_round(uint32_t len, uint32_t block)
{
	return block ? (uint32_t)(((uint64_t)len + block - 1) / block * block) : len;
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
