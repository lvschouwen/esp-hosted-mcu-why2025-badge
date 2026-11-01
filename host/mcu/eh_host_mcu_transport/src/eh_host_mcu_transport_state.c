/* SPDX-License-Identifier: Apache-2.0 */
#include <stdint.h>

#include "eh_host_mcu_transport_state.h"

static volatile uint8_t s_state = (uint8_t)EH_HOST_MCU_TRANSPORT_INACTIVE;
/* Latched by the bus drivers next to every TRANSPORT_FAILURE post; kept apart from
 * s_state so the drivers' own TX gating and the bring-up wait are unchanged. */
static volatile uint8_t s_failed;

eh_host_mcu_transport_state_t eh_host_transport_state_get(void)
{
    return (eh_host_mcu_transport_state_t)s_state;
}

void eh_host_mcu_transport_state_set(eh_host_mcu_transport_state_t state)
{
    /* Any transition is a (re)initialised bus: an earlier failure no longer applies. */
    s_failed = 0;
    s_state = (uint8_t)state;
}

int eh_host_mcu_transport_state_is_rx_ready(void)
{
    return s_state >= (uint8_t)EH_HOST_MCU_TRANSPORT_RX_ACTIVE;
}

int eh_host_mcu_transport_state_is_tx_ready(void)
{
    return s_state >= (uint8_t)EH_HOST_MCU_TRANSPORT_TX_ACTIVE;
}

void eh_host_mcu_transport_state_set_failed(void)
{
    s_failed = 1;
}

void eh_host_mcu_transport_state_note_rx(void)
{
    if (s_failed) {
        s_failed = 0;
    }
}

int eh_host_mcu_transport_state_is_failed(void)
{
    return s_failed != 0;
}
