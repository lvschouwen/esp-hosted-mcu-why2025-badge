/* SPDX-License-Identifier: Apache-2.0 */
/* Slicing of a sync RPC wait (ESP-free, host-testable). */

#ifndef EH_HOST_FEAT_RPC_SLICE_H_
#define EH_HOST_FEAT_RPC_SLICE_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Same value as EH_HOST_PORT_WAIT_FOREVER: no time limit. */
#define EH_RPC_WAIT_FOREVER ((uint32_t)0xFFFFFFFFu)

/* Next step of a wait with *left ms remaining (EH_RPC_WAIT_FOREVER: no limit).
 * Returns the time to wait now and takes it off a bounded *left. *more is true
 * when another slice may follow: only then is the transport checked between
 * slices. A zero wait is a single poll. */
uint32_t eh_rpc_next_slice(uint32_t *left, uint32_t poll_ms, bool *more);

#ifdef __cplusplus
}
#endif

#endif /* EH_HOST_FEAT_RPC_SLICE_H_ */
