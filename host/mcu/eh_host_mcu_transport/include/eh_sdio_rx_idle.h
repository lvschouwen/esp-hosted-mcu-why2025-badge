/*
 * SDIO RX staging: when to hand idle slots back (pure, ESP-free, host-tested).
 *
 * The staging slots only grow (sdio_rx_get_buffer) to the largest aggregate
 * the slave ever queued, up to 14 x 1536 B per slot, in scarce internal DMA
 * RAM. Once the link has been quiet they are released down to a small floor.
 */
#ifndef EH_SDIO_RX_IDLE_H
#define EH_SDIO_RX_IDLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Slots at or below this stay allocated: it covers the ordinary single-frame
 * reads (<= 1536 B + header), so idle traffic never reallocates. */
#define EH_SDIO_RX_KEEP_FLOOR  2048u

/* No NEW_PACKET for this long = quiet. Far above the gap between reads inside
 * a stream (back to back, the slave refills its queue during the parse), so a
 * stream never trips it; short enough that a finished push gives the RAM back
 * before the next app needs it. */
#define EH_SDIO_RX_IDLE_MS     250u

/* Release only when the link was quiet for the whole threshold and no slot is
 * staged for the parse task (pending == 0), else a slot could be freed under
 * the parser. */
static inline bool eh_sdio_rx_release_due(int pending, uint32_t quiet_ms)
{
	return pending == 0 && quiet_ms >= EH_SDIO_RX_IDLE_MS;
}

static inline bool eh_sdio_rx_slot_over_floor(size_t buf_size)
{
	return buf_size > EH_SDIO_RX_KEEP_FLOOR;
}

/* Which of the n slots (size 0 = nothing allocated) to free now: bit i of
 * *free_mask. Returns the bytes those slots hold; 0 and an empty mask when the
 * release is not due. The caller frees and clears the slots it is told to. */
static inline size_t eh_sdio_rx_release_plan(const size_t *sizes, unsigned n,
					     int pending, uint32_t quiet_ms,
					     uint32_t *free_mask)
{
	size_t bytes = 0;

	*free_mask = 0;
	if (!eh_sdio_rx_release_due(pending, quiet_ms))
		return 0;
	for (unsigned i = 0; i < n && i < 32; i++) {
		if (eh_sdio_rx_slot_over_floor(sizes[i])) {
			*free_mask |= 1u << i;
			bytes += sizes[i];
		}
	}
	return bytes;
}

#endif
