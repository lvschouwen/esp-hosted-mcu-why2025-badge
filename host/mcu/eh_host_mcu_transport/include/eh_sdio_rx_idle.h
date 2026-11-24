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
static inline bool eh_sdio_rx_release_due(uint32_t pending, uint32_t quiet_ms)
{
	return pending == 0 && quiet_ms >= EH_SDIO_RX_IDLE_MS;
}

static inline bool eh_sdio_rx_slot_over_floor(size_t buf_size)
{
	return buf_size > EH_SDIO_RX_KEEP_FLOOR;
}

#endif
