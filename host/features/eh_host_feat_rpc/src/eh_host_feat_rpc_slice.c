/* SPDX-License-Identifier: Apache-2.0 */
/* Slicing of a sync RPC wait (ESP-free, host-testable). */

#include "eh_host_feat_rpc_slice.h"

uint32_t eh_rpc_next_slice(uint32_t *left, uint32_t poll_ms, bool *more)
{
    if (*left == EH_RPC_WAIT_FOREVER) {
        *more = true;   /* forever: slice it too, so a dead transport ends it */
        return poll_ms;
    }
    if (*left > poll_ms) {
        *left -= poll_ms;
        *more = true;
        return poll_ms;
    }
    uint32_t slice = *left;
    *left = 0;
    *more = false;
    return slice;
}
