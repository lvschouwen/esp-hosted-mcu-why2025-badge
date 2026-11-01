/* SPDX-License-Identifier: Apache-2.0 */
/* Transport state machine: INACTIVE -> RX_ACTIVE -> TX_ACTIVE. */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    EH_HOST_MCU_TRANSPORT_INACTIVE  = 0,
    EH_HOST_MCU_TRANSPORT_RX_ACTIVE = 1,    /* RX ready, TX not yet */
    EH_HOST_MCU_TRANSPORT_TX_ACTIVE = 2,    /* fully up: both RX and TX */
} eh_host_mcu_transport_state_t;

eh_host_mcu_transport_state_t eh_host_transport_state_get(void);
void                      eh_host_mcu_transport_state_set(eh_host_mcu_transport_state_t state);

int eh_host_mcu_transport_state_is_rx_ready(void);
int eh_host_mcu_transport_state_is_tx_ready(void);

/* Transport-failure latch. The bus driver sets it when it reports
 * EH_HOST_EVENT_TRANSPORT_FAILURE; any state transition (bus deinit/init, the
 * slave's INIT event) and any frame received from the slave clear it. Callers
 * waiting on the slave (RPC) use it to fail at once instead of timing out. */
void eh_host_mcu_transport_state_set_failed(void);
void eh_host_mcu_transport_state_note_rx(void);
int  eh_host_mcu_transport_state_is_failed(void);

#ifdef __cplusplus
}
#endif
